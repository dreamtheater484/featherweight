#include "doc/edit/Pictures.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "doc/docx/Docx.h"
#include "doc/model/Internal.h"
#include "draw/Images.h"
#include "pkg/Package.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"

namespace fw::doc::edit {

// The document's drawing handles, which only the model's friends change.
struct PictureAccess {
  static Drawing& At(Document& d, uint32_t i) { return d.drawings_[i]; }
};

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::string_view kWp = "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing";
constexpr std::string_view kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view kPic = "http://schemas.openxmlformats.org/drawingml/2006/picture";
constexpr std::string_view kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view kWp14 = "http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing";
constexpr std::string_view kAsvg = "http://schemas.microsoft.com/office/drawing/2016/SVG/main";
constexpr std::string_view kImageRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr const char* kSvgExt = "{96DAC541-7B7A-43D3-8B79-37D633B846F1}";
constexpr int64_t kEmuPerPoint = 12700;
// Word's limits for a picture's size: 1/100 inch to 22 inches.
constexpr int64_t kMinEmu = 9144, kMaxEmu = 22 * 914400;

std::u16string Msg(const char* s) { return std::u16string(s, s + std::char_traits<char>::length(s)); }

Result Fail(std::u16string why) {
  Result r;
  r.ok = false;
  r.message = std::move(why);
  return r;
}

uint32_t g_version = 0;  // Drawing::version: every change gets a new one

// ---------- A drawing's XML ----------

// One picture's wp:inline or wp:anchor, parsed for a change, with the w:drawing's fragments.
struct Frame {
  pkg::XmlDom dom;
  pugi::xml_node root;  // wp:inline or wp:anchor
  pugi::xml_node pic;   // pic:pic
  size_t index = 0;     // which of the fragments it is
  std::vector<pkg::Fragment> fragments;
  std::string wp, a;    // the prefixes it uses for them, with the colon ("wp:"), or empty

  std::string Wp(std::string_view local) const { return wp + std::string(local); }
  std::string A(std::string_view local) const { return a + std::string(local); }
};

pugi::xml_node Child(pugi::xml_node parent, std::string_view ns, std::string_view local) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling())
    if (pkg::IsElement(c, ns, local)) return c;
  return {};
}

std::string PrefixOf(pugi::xml_node element) {
  std::string_view name = element.name();
  size_t colon = name.find(':');
  return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon + 1));
}

// The prefix `ns` has where `node` is, declaring `preferred` on `root` when it has none.
std::string PrefixFor(pugi::xml_node node, pugi::xml_node root, std::string_view ns, const char* preferred) {
  for (pugi::xml_node n = node; n; n = n.parent())
    for (pugi::xml_attribute at : n.attributes()) {
      std::string_view name = at.name();
      if (at.value() != ns) continue;
      if (name == "xmlns") return {};
      if (name.substr(0, 6) == "xmlns:") return std::string(name.substr(6)) + ":";
    }
  std::string decl = std::string("xmlns:") + preferred;
  if (!root.attribute(decl.c_str())) root.append_attribute(decl.c_str()) = std::string(ns).c_str();
  return std::string(preferred) + ":";
}

const Part* PartOf(const Document& doc, uint32_t part) { return part < doc.Parts().size() ? &doc.Parts()[part] : nullptr; }

bool Strict(const Part& part) {
  return part.uri[static_cast<size_t>(Ns::W)].find("purl.oclc.org") != std::string::npos;
}

// Parses drawing `i` for a change; false with the reason when it isn't a picture this can change.
bool Load(const Editor& editor, uint32_t i, Frame& f, std::u16string& why) {
  const Document& doc = editor.Doc();
  if (!editor.CanEdit(&why)) return false;
  if (i >= doc.Drawings().size()) {
    why = u"That picture isn't in the document any more.";
    return false;
  }
  const Drawing& d = doc.Drawings()[i];
  if (d.pos.para == kNone || !d.node) {
    why = u"That picture isn't in the document any more.";
    return false;
  }
  if (d.kind != DrawingKind::DrawingML) {
    why = d.kind == DrawingKind::Vml || d.kind == DrawingKind::Object
              ? u"This picture is in an older format, so it can't be changed. You can delete it and insert it again."
              : u"Only pictures can be changed.";
    return false;
  }
  if (d.displayOnly || !d.keptXml.empty()) {
    why = u"This picture has a second version for older Word versions, so it can't be changed yet.";
    return false;
  }
  const Part* part = PartOf(doc, d.part);
  if (!part || !part->Editable() || !part->writable) {
    why = u"This part of the document uses features Featherweight doesn't understand, so it can't be edited.";
    return false;
  }
  const Story& story = doc.Stories()[doc.Para(d.pos.para).story];
  if (story.drawing != kNone) {
    why = u"Pictures in a text box can't be changed yet.";
    return false;
  }
  f.fragments = doc.Store().FragmentsCopy(d.node.Raw().node);
  bool found = false;
  for (size_t k = 0; k < f.fragments.size() && !found; ++k) {
    const pkg::Fragment& frag = f.fragments[k];
    if (frag.slot != pkg::Slot::Inside || frag.xml.find('<') == std::string::npos) continue;
    if (!f.dom.Parse(frag.xml, doc.Package().GetLimits()).Ok()) continue;
    pugi::xml_node root = f.dom.Root();
    if (pkg::IsElement(root, kWp, "inline") || pkg::IsElement(root, kWp, "anchor")) {
      f.root = root;
      f.index = k;
      found = true;
    }
  }
  if (!found) {
    why = u"Only pictures can be changed.";
    return false;
  }
  pugi::xml_node data = Child(Child(f.root, kA, "graphic"), kA, "graphicData");
  f.pic = Child(data, kPic, "pic");
  if (!f.pic) {
    why = u"Only pictures can be changed. This is a shape, a chart or another kind of drawing.";
    return false;
  }
  f.wp = PrefixOf(f.root);
  f.a = PrefixOf(Child(f.root, kA, "graphic"));
  return true;
}

// The handle a frame makes, keeping what isn't its placement from `was`.
Drawing HandleFor(const Drawing& was, pugi::xml_node root) {
  Drawing d;
  d.kind = was.kind;
  d.node = was.node;
  d.pos = was.pos;
  d.part = was.part;
  d.textBoxes = was.textBoxes;
  docx::ReadDrawingFrame(root, d);
  return d;
}

// ---------- Steps ----------

// A change to a drawing's XML and its handle. Applied when made; undo and redo swap.
struct DrawingStep : CustomStep {
  Document* doc = nullptr;
  uint32_t drawing = 0;
  pkg::NodeId node = pkg::kNoNode;
  std::vector<pkg::Fragment> before, after;
  Drawing handleBefore, handleAfter;

  void Apply(const std::vector<pkg::Fragment>& frags, const Drawing& handle) {
    doc->Store().SetFragments(node, frags);
    Drawing& h = PictureAccess::At(*doc, drawing);
    Drawing keep = h;
    h = handle;
    h.node = keep.node;
    h.pos = keep.pos;
    h.part = keep.part;
    h.kind = keep.kind;
    h.textBoxes = keep.textBoxes;
    h.keptXml = keep.keptXml;
    h.displayOnly = keep.displayOnly;
    h.version = ++g_version;
  }
  void Undo() override { Apply(before, handleBefore); }
  void Redo() override { Apply(after, handleAfter); }
};

// Package changes: parts added or changed, a relationship set, the content types.
struct PackageStep : CustomStep {
  Document* doc = nullptr;
  struct PartState {
    std::string name;
    std::optional<std::vector<uint8_t>> before, after;  // nothing: no such part
  };
  std::vector<PartState> partStates;
  std::optional<pkg::RelationshipSet> relsBefore, relsAfter;
  std::optional<pkg::ContentTypes> typesBefore, typesAfter;

  void Apply(bool undo) {
    pkg::Package& p = doc->Package();
    for (const PartState& s : partStates) {
      const auto& v = undo ? s.before : s.after;
      if (v) p.SetPart(s.name, *v);
      else p.DeletePart(s.name);
    }
    if (const auto& r = undo ? relsBefore : relsAfter) p.SetRelationships(*r);
    if (const auto& t = undo ? typesBefore : typesAfter) p.SetContentTypes(*t);
  }
  void Undo() override { Apply(true); }
  void Redo() override { Apply(false); }
};

// ---------- Geometry ----------

int64_t ClampEmu(int64_t v) { return std::clamp<int64_t>(v, kMinEmu, kMaxEmu); }

int64_t AttrInt(pugi::xml_node n, const char* name, int64_t fallback = 0) {
  pugi::xml_attribute at = n.attribute(name);
  if (!at) return fallback;
  char* end = nullptr;
  long long v = std::strtoll(at.value(), &end, 10);
  return end && end != at.value() ? v : fallback;
}

void SetInt(pugi::xml_node n, const char* name, int64_t v) {
  pugi::xml_attribute at = n.attribute(name);
  if (!at) at = n.append_attribute(name);
  at.set_value(std::to_string(v).c_str());
}

// pic:spPr's a:xfrm, made with a:off and a:ext when missing (`create`).
pugi::xml_node Xfrm(Frame& f, bool create) {
  pugi::xml_node spPr = Child(f.pic, kPic, "spPr");
  if (!spPr && create) {
    std::string name = PrefixOf(f.pic) + "spPr";
    pugi::xml_node blipFill = Child(f.pic, kPic, "blipFill");
    spPr = blipFill ? f.pic.insert_child_after(name.c_str(), blipFill) : f.pic.append_child(name.c_str());
  }
  if (!spPr) return {};
  pugi::xml_node x = Child(spPr, kA, "xfrm");
  if (!x && create) {
    x = spPr.prepend_child(f.A("xfrm").c_str());
    pugi::xml_node off = x.append_child(f.A("off").c_str());
    SetInt(off, "x", 0);
    SetInt(off, "y", 0);
    pugi::xml_node ext = x.append_child(f.A("ext").c_str());
    SetInt(ext, "cx", 0);
    SetInt(ext, "cy", 0);
  }
  return x;
}

int64_t RotationOf(Frame& f) {
  pugi::xml_node x = Xfrm(f, false);
  return x ? AttrInt(x, "rot") : 0;
}

// What a turned box needs beyond its own size on each side: (x, y).
std::pair<int64_t, int64_t> TurnRoom(int64_t cx, int64_t cy, int64_t rot) {
  double t = static_cast<double>(rot) / 60000.0 * 3.14159265358979323846 / 180.0;
  double c = std::fabs(std::cos(t)), s = std::fabs(std::sin(t));
  double w = cx * c + cy * s, h = cx * s + cy * c;
  return {std::max<int64_t>(0, std::llround((w - cx) / 2)), std::max<int64_t>(0, std::llround((h - cy) / 2))};
}

pugi::xml_node InsertAfterFirst(pugi::xml_node parent, const std::string& name, std::initializer_list<std::pair<std::string_view, std::string_view>> after) {
  pugi::xml_node prev;
  for (auto [ns, local] : after)
    if (pugi::xml_node n = Child(parent, ns, local)) prev = n;
  return prev ? parent.insert_child_after(name.c_str(), prev) : parent.prepend_child(name.c_str());
}

// Sets the size and turn, keeping wp:extent, the picture's a:ext and the room around it in step.
void SetGeometry(Frame& f, int64_t cx, int64_t cy, int64_t rot) {
  pugi::xml_node extent = Child(f.root, kWp, "extent");
  if (!extent) extent = f.root.prepend_child(f.Wp("extent").c_str());
  int64_t oldCx = AttrInt(extent, "cx"), oldCy = AttrInt(extent, "cy"), oldRot = RotationOf(f);
  SetInt(extent, "cx", cx);
  SetInt(extent, "cy", cy);
  pugi::xml_node x = Xfrm(f, true);
  if (x) {
    pugi::xml_node ext = Child(x, kA, "ext");
    if (!ext) ext = x.append_child(f.A("ext").c_str());
    SetInt(ext, "cx", cx);
    SetInt(ext, "cy", cy);
    if (rot != 0) SetInt(x, "rot", rot);
    else x.remove_attribute("rot");
  }
  // The room around it: what it had less what its old turn needed, plus what the new one needs.
  auto [ox, oy] = TurnRoom(oldCx, oldCy, oldRot);
  auto [nx, ny] = TurnRoom(cx, cy, rot);
  pugi::xml_node effect = Child(f.root, kWp, "effectExtent");
  if (!effect && nx == 0 && ny == 0) return;
  if (!effect) {
    effect = InsertAfterFirst(f.root, f.Wp("effectExtent"), {{kWp, "extent"}});
    for (const char* side : {"l", "t", "r", "b"}) SetInt(effect, side, 0);
  }
  auto side = [&](const char* name, int64_t was, int64_t now) {
    SetInt(effect, name, std::max<int64_t>(0, AttrInt(effect, name) - was + now));
  };
  side("l", ox, nx);
  side("r", ox, nx);
  side("t", oy, ny);
  side("b", oy, ny);
}

Crop CropOf(Frame& f) {
  Crop c;
  pugi::xml_node src = Child(Child(f.pic, kPic, "blipFill"), kA, "srcRect");
  if (!src) return c;
  c.left = AttrInt(src, "l") / 100000.0;
  c.top = AttrInt(src, "t") / 100000.0;
  c.right = AttrInt(src, "r") / 100000.0;
  c.bottom = AttrInt(src, "b") / 100000.0;
  return c;
}

bool ValidCrop(const Crop& c) {
  for (double v : {c.left, c.top, c.right, c.bottom})
    if (!std::isfinite(v) || v < -1 || v > 0.99) return false;
  return 1 - c.left - c.right >= 0.01 && 1 - c.top - c.bottom >= 0.01;
}

void SetCropXml(Frame& f, const Crop& c) {
  pugi::xml_node fill = Child(f.pic, kPic, "blipFill");
  if (!fill) return;
  pugi::xml_node src = Child(fill, kA, "srcRect");
  if (!src && !c.Any()) return;
  if (!src) src = InsertAfterFirst(fill, f.A("srcRect"), {{kA, "blip"}});
  auto set = [&](const char* name, double v) {
    int64_t n = std::llround(v * 100000);
    if (n != 0) SetInt(src, name, n);
    else src.remove_attribute(name);
  };
  set("l", c.left);
  set("t", c.top);
  set("r", c.right);
  set("b", c.bottom);
}

// ---------- Wrapping ----------

constexpr const char* kWraps[] = {"wrapNone", "wrapSquare", "wrapTight", "wrapThrough", "wrapTopAndBottom"};

void SetWrapXml(Frame& f, Wrap wrap) {
  for (const char* w : kWraps)
    while (pugi::xml_node n = Child(f.root, kWp, w)) f.root.remove_child(n);
  const char* name = wrap == Wrap::Square ? "wrapSquare" : wrap == Wrap::Tight ? "wrapTight"
                     : wrap == Wrap::TopAndBottom ? "wrapTopAndBottom" : "wrapNone";
  pugi::xml_node w = InsertAfterFirst(f.root, f.Wp(name), {{kWp, "simplePos"}, {kWp, "positionH"}, {kWp, "positionV"}, {kWp, "extent"}, {kWp, "effectExtent"}});
  if (wrap == Wrap::Square || wrap == Wrap::Tight) w.append_attribute("wrapText") = "bothSides";
  if (wrap == Wrap::Tight) {
    // Its box, in the polygon's 21600 units, as Word writes for a picture.
    pugi::xml_node poly = w.append_child(f.Wp("wrapPolygon").c_str());
    poly.append_attribute("edited") = "0";
    const int pts[][2] = {{0, 0}, {0, 21600}, {21600, 21600}, {21600, 0}, {0, 0}};
    for (size_t k = 0; k < std::size(pts); ++k) {
      pugi::xml_node p = poly.append_child(f.Wp(k == 0 ? "start" : "lineTo").c_str());
      SetInt(p, "x", pts[k][0]);
      SetInt(p, "y", pts[k][1]);
    }
  }
  pugi::xml_attribute behind = f.root.attribute("behindDoc");
  if (!behind) behind = f.root.append_attribute("behindDoc");
  behind = wrap == Wrap::Behind ? "1" : "0";
}

// The wp:anchor's position on its page: page-relative offsets.
void SetPosition(Frame& f, int64_t x, int64_t y) {
  for (const char* axis : {"positionH", "positionV"}) {
    pugi::xml_node pos = Child(f.root, kWp, axis);
    if (!pos) pos = InsertAfterFirst(f.root, f.Wp(axis), {{kWp, "simplePos"}, {kWp, "positionH"}});
    pos.remove_children();
    pugi::xml_attribute rel = pos.attribute("relativeFrom");
    if (!rel) rel = pos.append_attribute("relativeFrom");
    rel = "page";
    pos.append_child(f.Wp("posOffset").c_str()).text() = std::to_string(axis[8] == 'H' ? x : y).c_str();
  }
  // Placed by the offsets, not simplePos.
  pugi::xml_attribute simple = f.root.attribute("simplePos");
  if (!simple) simple = f.root.append_attribute("simplePos");
  simple = "0";
}

int64_t TopZ(const Document& doc) {
  int64_t z = 251658240;  // where Word starts
  for (const Drawing& d : doc.Drawings())
    if (d.anchored) z = std::max(z, d.relativeHeight);
  return std::min<int64_t>(z + 1024, 0x7FFFFFFF);
}

// In line to floating: wp:inline becomes wp:anchor, placed at (x, y) on the page.
void ToAnchor(Frame& f, const Document& doc, Wrap wrap, int64_t x, int64_t y) {
  f.root.set_name(f.Wp("anchor").c_str());
  bool square = wrap == Wrap::Square || wrap == Wrap::Tight;
  for (const char* side : {"distT", "distB"}) SetInt(f.root, side, 0);
  for (const char* side : {"distL", "distR"}) SetInt(f.root, side, square ? 114300 : 0);
  SetInt(f.root, "relativeHeight", TopZ(doc));
  f.root.append_attribute("behindDoc") = "0";
  f.root.append_attribute("locked") = "0";
  f.root.append_attribute("layoutInCell") = "1";
  f.root.append_attribute("allowOverlap") = "1";
  pugi::xml_node sp = f.root.prepend_child(f.Wp("simplePos").c_str());
  SetInt(sp, "x", 0);
  SetInt(sp, "y", 0);
  SetPosition(f, x, y);
  SetWrapXml(f, wrap);
}

// Floating to in line; false when the anchor holds something an inline picture can't.
bool ToInline(Frame& f, std::u16string& why) {
  for (pugi::xml_node c = f.root.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view local = pkg::LocalNameOf(c), ns = pkg::NamespaceOf(c);
    bool known = (ns == kWp && (local == "simplePos" || local == "positionH" || local == "positionV" || local == "extent" ||
                                local == "effectExtent" || local == "docPr" || local == "cNvGraphicFramePr" ||
                                std::find_if(std::begin(kWraps), std::end(kWraps), [&](const char* w) { return local == w; }) != std::end(kWraps))) ||
                 (ns == kA && local == "graphic") || (ns == kWp14 && (local == "sizeRelH" || local == "sizeRelV"));
    if (!known) {
      why = u"This picture has settings Featherweight doesn't know, so it stays where it is.";
      return false;
    }
  }
  f.root.set_name(f.Wp("inline").c_str());
  for (const char* at : {"simplePos", "relativeHeight", "behindDoc", "locked", "layoutInCell", "allowOverlap", "hidden"})
    f.root.remove_attribute(at);
  for (const char* side : {"distT", "distB", "distL", "distR"}) SetInt(f.root, side, 0);
  std::vector<pugi::xml_node> drop;
  for (pugi::xml_node c = f.root.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    std::string_view local = pkg::LocalNameOf(c);
    if (pkg::NamespaceOf(c) == kWp14 || local == "simplePos" || local == "positionH" || local == "positionV" ||
        std::find_if(std::begin(kWraps), std::end(kWraps), [&](const char* w) { return local == w; }) != std::end(kWraps))
      drop.push_back(c);
  }
  for (pugi::xml_node c : drop) f.root.remove_child(c);
  return true;
}

// ---------- Making a change ----------

std::string Serialize(const Frame& f) { return pkg::RoundTripStore::SerializeFragment(f.root); }

// Loads drawing `i`, lets `edit` change its XML, and records the change as one undo step.
Result ChangeDrawing(Editor& editor, uint32_t i, const char* name, const std::function<bool(Frame&, std::u16string&)>& edit) {
  Frame f;
  std::u16string why;
  if (!Load(editor, i, f, why)) return Fail(why);
  Document& doc = const_cast<Document&>(editor.Doc());
  const Drawing& was = doc.Drawings()[i];
  if (!edit(f, why)) return Fail(why);
  std::string xml = Serialize(f);
  if (!XmlSafe(xml)) return Fail(u"That can't be written in the document.");
  auto step = std::make_shared<DrawingStep>();
  step->doc = &doc;
  step->drawing = i;
  step->node = was.node.Raw().node;
  step->before = f.fragments;
  step->after = f.fragments;
  step->after[f.index].xml = std::move(xml);
  if (step->after[f.index].xml == step->before[f.index].xml) {
    Result r;
    r.caret = was.pos;
    return r;  // nothing changed
  }
  step->handleBefore = was;
  step->handleAfter = HandleFor(was, f.root);
  step->parts = {was.part};
  step->first = was.pos.para;
  step->count = 1;
  Pos at = was.pos;
  step->Redo();
  editor.RecordStep(std::u16string(Msg(name)), step, at);
  Result r;
  r.caret = at;
  return r;
}

// ---------- Inserting ----------

uint64_t Fnv(const std::vector<uint8_t>& b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
  return h;
}

// PNG bytes for premultiplied BGRA pixels; empty when WIC can't.
std::vector<uint8_t> EncodePng(const draw::Bitmap& bmp) {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> source;
  ComPtr<IWICFormatConverter> straight;
  ComPtr<IStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::vector<uint8_t> out;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
      SUCCEEDED(wic->CreateBitmapFromMemory(bmp.Width(), bmp.Height(), GUID_WICPixelFormat32bppPBGRA, bmp.Stride(),
                                            static_cast<UINT>(bmp.Stride()) * bmp.Height(), const_cast<BYTE*>(bmp.Pixels()), &source)) &&
      SUCCEEDED(wic->CreateFormatConverter(&straight)) &&
      SUCCEEDED(straight->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
      SUCCEEDED(CoCreateInstance(CLSID_WICPngEncoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder))) &&
      SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
      SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
      SUCCEEDED(frame->SetSize(bmp.Width(), bmp.Height())) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
      SUCCEEDED(frame->WriteSource(straight.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
    STATSTG st{};
    LARGE_INTEGER zero{};
    ULONG read = 0;
    if (SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)) && st.cbSize.QuadPart < (1ull << 31) &&
        SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
      out.resize(static_cast<size_t>(st.cbSize.QuadPart));
      if (FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) || read != out.size()) out.clear();
    }
  }
  if (SUCCEEDED(hrCom)) CoUninitialize();
  return out;
}

struct Media {
  std::vector<uint8_t> bytes;
  std::string ext, type;
};

// Where a picture's part goes, and the relationship to it, as a package step (null when the
// document already has both).
struct Placed {
  std::string relId;
  std::shared_ptr<PackageStep> step;
};

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string PlaceMedia(Document& doc, const Part& source, const Media& m, PackageStep& step) {
  pkg::Package& package = doc.Package();
  const std::string dir = source.name.substr(0, source.name.rfind('/') + 1);
  // The same picture already in the package: share its part.
  std::string partName;
  const uint64_t hash = Fnv(m.bytes);
  std::vector<uint8_t> have;
  for (const pkg::Part& p : package.Parts()) {
    if (p.deleted || package.ContentTypeOf(p.name).substr(0, 6) != "image/") continue;
    if (!package.ReadPart(p.name, have).Ok() || have.size() != m.bytes.size() || Fnv(have) != hash || have != m.bytes) continue;
    partName = p.name;
    break;
  }
  bool newPart = partName.empty();
  if (newPart) {
    // imageN, numbered across extensions as Word does.
    const std::string stem = Lower(dir + "media/image");
    auto taken = [&](int n) {
      std::string prefix = stem + std::to_string(n) + ".";
      for (const pkg::Part& p : package.Parts())
        if (!p.deleted && Lower(p.name).rfind(prefix, 0) == 0) return true;
      for (const auto& s : step.partStates)
        if (Lower(s.name).rfind(prefix, 0) == 0) return true;
      return false;
    };
    int n = 1;
    while (taken(n)) ++n;
    partName = dir + "media/image" + std::to_string(n) + "." + m.ext;
    step.partStates.push_back({partName, std::nullopt, m.bytes});
    // Its content type, by extension when the package has none for it, else for the part.
    pkg::ContentTypes types = step.typesAfter ? *step.typesAfter : package.Types();
    std::string_view byExt = types.LookupDefault(partName);
    if (byExt.empty()) types.defaults.emplace_back(m.ext, m.type);
    else if (byExt != m.type) types.overrides.emplace_back(partName, m.type);
    if (byExt != m.type) {
      if (!step.typesBefore) step.typesBefore = package.Types();
      step.typesAfter = std::move(types);
    }
  }
  // The relationship from the part the picture is in.
  pkg::RelationshipSet rels;
  if (step.relsAfter) rels = *step.relsAfter;
  else if (const pkg::RelationshipSet* r = package.RelationshipsOf(source.name)) rels = *r;
  else {
    rels.source = source.name;
    rels.relsPart = pkg::RelsPartFor(source.name);
  }
  for (const pkg::Relationship& r : rels.items)
    if (!r.external && r.type == kImageRel && Lower(r.targetPart) == Lower(partName)) return r.id;
  pkg::Relationship rel;
  for (size_t n = rels.items.size() + 1;; ++n) {
    rel.id = "rId" + std::to_string(n);
    if (!rels.Find(rel.id)) break;
  }
  rel.type = std::string(kImageRel);
  rel.target = partName.substr(0, dir.size()) == dir ? partName.substr(dir.size()) : partName;
  rel.targetPart = partName;
  std::string id = rel.id;
  if (!step.relsBefore) {
    if (const pkg::RelationshipSet* r = package.RelationshipsOf(source.name)) step.relsBefore = *r;
    else step.relsBefore = rels;  // none before: undone to an empty set
  }
  rels.items.push_back(std::move(rel));
  step.relsAfter = std::move(rels);
  return id;
}

std::string Escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string Utf8(std::u16string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c < 0xE000) {
      continue;  // a lone surrogate
    }
    if (c < 0x20 && c != '\t') continue;  // control characters can't be written
    if (c == 0xFFFE || c == 0xFFFF) continue;
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

std::u16string Utf16(std::string_view s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    auto b = static_cast<unsigned char>(s[i]);
    uint32_t c = b;
    size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    if (n > 1 && i + n <= s.size()) {
      c = b & (0x7F >> n);
      for (size_t k = 1; k < n; ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += n;
    if (c >= 0x10000) {
      c -= 0x10000;
      out += static_cast<char16_t>(0xD800 + (c >> 10));
      out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
    } else {
      out += static_cast<char16_t>(c);
    }
  }
  return out;
}

// The drawing's picture: its blip relationship id.
std::string BlipId(Frame& f) {
  pugi::xml_node blip = Child(Child(f.pic, kPic, "blipFill"), kA, "blip");
  pugi::xml_attribute at = pkg::FindAttribute(blip, kR, "embed");
  return at ? at.value() : std::string();
}

}  // namespace

// ---------- Pictures ----------

Pictures::Pictures(Editor& editor) : editor_(editor) {}

bool Pictures::Accepts(const std::vector<uint8_t>& bytes) {
  switch (draw::SniffImageFormat(bytes.data(), bytes.size())) {
    case draw::ImageFormat::Png:
    case draw::ImageFormat::Jpeg:
    case draw::ImageFormat::Gif:
    case draw::ImageFormat::Bmp:
    case draw::ImageFormat::Tiff:
    case draw::ImageFormat::Svg: return true;
    default: return false;
  }
}

Result Pictures::Insert(Pos at, const std::vector<uint8_t>& bytes, Emu maxWidth, std::u16string_view name) {
  Document& doc = const_cast<Document&>(editor_.Doc());
  std::u16string why;
  if (!editor_.CanEdit(&why)) return Fail(why);
  if (at.para >= doc.Paragraphs().size()) return Fail(u"That place isn't in the document.");
  const Story& story = doc.Stories()[doc.Para(at.para).story];
  const uint32_t storyPart = story.part;
  const Part* part = PartOf(doc, storyPart);
  if (!part || story.drawing != kNone) return Fail(u"A picture can't go here.");
  if (Strict(*part)) return Fail(u"Pictures can't be added to documents saved as Strict Open XML yet.");
  if (!Accepts(bytes)) return Fail(u"This isn't a picture Featherweight can add. Use a PNG, JPEG, GIF, BMP, TIFF or SVG file.");

  // Read it as anything from outside is: its header, within the limits.
  std::string error;
  std::shared_ptr<draw::Image> image = draw::Image::Open(bytes, draw::ImageLimits::Default(), &error);
  if (!image) return Fail(error.empty() ? u"This picture can't be read." : Utf16(error));
  draw::Size natural = image->NaturalSize();
  if (!(natural.width > 0 && natural.height > 0 && std::isfinite(natural.width) && std::isfinite(natural.height)))
    return Fail(u"This picture has no size.");

  // What goes in the package.
  Media main, svg;
  bool isSvg = image->Format() == draw::ImageFormat::Svg;
  switch (image->Format()) {
    case draw::ImageFormat::Png: main = {bytes, "png", "image/png"}; break;
    case draw::ImageFormat::Jpeg: main = {bytes, "jpeg", "image/jpeg"}; break;
    case draw::ImageFormat::Gif: main = {bytes, "gif", "image/gif"}; break;
    case draw::ImageFormat::Tiff: main = {bytes, "tiff", "image/tiff"}; break;
    case draw::ImageFormat::Bmp: {
      std::shared_ptr<const draw::Bitmap> px = image->Pixels();
      std::vector<uint8_t> png = px ? EncodePng(*px) : std::vector<uint8_t>{};
      main = png.empty() ? Media{bytes, "bmp", "image/bmp"} : Media{std::move(png), "png", "image/png"};
      break;
    }
    case draw::ImageFormat::Svg: {
      // Word before 2016 shows a PNG copy: at its own size, twice over for sharpness, within 2048.
      svg = {bytes, "svg", "image/svg+xml"};
      double k = std::min(2.0 * 96 / 72, 2048.0 / std::max(natural.width, natural.height) * 0.999);
      int w = std::max(1, static_cast<int>(natural.width * k)), h = std::max(1, static_cast<int>(natural.height * k));
      std::shared_ptr<const draw::Bitmap> px = image->Vector() ? draw::RasterizeVectorImage(*image->Vector(), w, h) : nullptr;
      std::vector<uint8_t> png = px ? EncodePng(*px) : std::vector<uint8_t>{};
      if (png.empty()) return Fail(u"This SVG picture can't be drawn, so it can't be added.");
      main = {std::move(png), "png", "image/png"};
      break;
    }
    default: return Fail(u"This isn't a picture Featherweight can add.");
  }

  // Its size: its own, made smaller to fit the column.
  int64_t cx = std::llround(natural.width * kEmuPerPoint), cy = std::llround(natural.height * kEmuPerPoint);
  if (maxWidth.Value() > 0 && cx > maxWidth.Value()) {
    cy = std::max<int64_t>(1, std::llround(static_cast<double>(cy) * maxWidth.Value() / cx));
    cx = maxWidth.Value();
  }
  if (std::max(cx, cy) > kMaxEmu) {
    double k = static_cast<double>(kMaxEmu) / std::max(cx, cy);
    cx = std::llround(cx * k);
    cy = std::llround(cy * k);
  }
  cx = ClampEmu(cx);
  cy = ClampEmu(cy);

  auto step = std::make_shared<PackageStep>();
  step->doc = &doc;
  std::string rid = PlaceMedia(doc, *part, main, *step);
  std::string svgRid = isSvg ? PlaceMedia(doc, *part, svg, *step) : std::string();

  // Its docPr id: one no drawing has.
  uint32_t id = 1;
  for (const Drawing& d : doc.Drawings()) id = std::max(id, d.id + 1);
  std::string fileName = Utf8(name);
  if (fileName.empty()) fileName = "Picture " + std::to_string(id);
  std::string blipExt;
  if (isSvg)
    blipExt = std::string("<a:extLst><a:ext uri=\"") + kSvgExt + "\"><asvg:svgBlip xmlns:asvg=\"" + std::string(kAsvg) +
              "\" r:embed=\"" + svgRid + "\"/></a:ext></a:extLst>";
  std::string xml =
      "<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\" xmlns:wp=\"" + std::string(kWp) + "\" xmlns:a=\"" +
      std::string(kA) + "\" xmlns:pic=\"" + std::string(kPic) + "\" xmlns:r=\"" + std::string(kR) + "\">" +
      "<wp:extent cx=\"" + std::to_string(cx) + "\" cy=\"" + std::to_string(cy) + "\"/>" +
      "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>" +
      "<wp:docPr id=\"" + std::to_string(id) + "\" name=\"Picture " + std::to_string(id) + "\"/>" +
      "<wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>" +
      "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic>" +
      "<pic:nvPicPr><pic:cNvPr id=\"0\" name=\"" + Escape(fileName) + "\"/><pic:cNvPicPr><a:picLocks noChangeAspect=\"1\" noChangeArrowheads=\"1\"/></pic:cNvPicPr></pic:nvPicPr>" +
      "<pic:blipFill><a:blip r:embed=\"" + rid + "\">" + blipExt + "</a:blip><a:stretch><a:fillRect/></a:stretch></pic:blipFill>" +
      "<pic:spPr bwMode=\"auto\"><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + std::to_string(cx) + "\" cy=\"" +
      std::to_string(cy) + "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>" +
      "</pic:pic></a:graphicData></a:graphic></wp:inline>";
  pkg::XmlDom dom;
  if (!dom.Parse(xml, doc.Package().GetLimits()).Ok()) return Fail(u"This picture can't be added.");
  Drawing info;
  info.kind = DrawingKind::DrawingML;
  docx::ReadDrawingFrame(dom.Root(), info);

  editor_.BeginGroup(u"Insert picture");
  Result r = editor_.InsertDrawing(at, xml, info);
  if (r && (step->relsAfter || !step->partStates.empty() || step->typesAfter)) {
    step->parts = {storyPart};
    step->first = at.para;
    step->count = 1;
    step->Redo();
    editor_.RecordStep(u"Insert picture", step, at);
  }
  editor_.EndGroup();
  if (r) r.caret = at;  // the picture's own character: the view selects it
  return r;
}

PictureInfo Pictures::Info(uint32_t drawing) const {
  PictureInfo info;
  Frame f;
  if (!Load(editor_, drawing, f, info.why)) return info;
  const Drawing& d = editor_.Doc().Drawings()[drawing];
  info.picture = true;
  info.cx = d.cx;
  info.cy = d.cy;
  if (!d.anchored) info.wrap = Wrap::Inline;
  else if (d.wrap == WrapType::None) info.wrap = d.behindDoc ? Wrap::Behind : Wrap::InFront;
  else if (d.wrap == WrapType::Square) info.wrap = Wrap::Square;
  else if (d.wrap == WrapType::TopAndBottom) info.wrap = Wrap::TopAndBottom;
  else info.wrap = Wrap::Tight;
  pugi::xml_node x = Xfrm(f, false);
  double rot = x ? AttrInt(x, "rot") / 60000.0 : 0;
  info.rotation = std::fmod(std::fmod(rot, 360.0) + 360.0, 360.0);
  info.flipH = x && (std::string_view(x.attribute("flipH").value()) == "1" || std::string_view(x.attribute("flipH").value()) == "true");
  info.flipV = x && (std::string_view(x.attribute("flipV").value()) == "1" || std::string_view(x.attribute("flipV").value()) == "true");
  info.crop = CropOf(f);
  for (pugi::xml_node locks : {Child(Child(f.root, kWp, "cNvGraphicFramePr"), kA, "graphicFrameLocks"),
                               Child(Child(Child(f.pic, kPic, "nvPicPr"), kPic, "cNvPicPr"), kA, "picLocks")}) {
    std::string_view v = locks.attribute("noChangeAspect").value();
    if (v == "0" || v == "false") info.lockAspect = false;
  }
  info.altText = Utf16(d.descr);
  return info;
}

Result Pictures::Resize(uint32_t drawing, Emu cx, Emu cy) {
  return ChangeDrawing(editor_, drawing, "Resize picture", [&](Frame& f, std::u16string&) {
    SetGeometry(f, ClampEmu(cx.Value()), ClampEmu(cy.Value()), RotationOf(f));
    return true;
  });
}

Result Pictures::Rotate(uint32_t drawing, double degrees) {
  if (!std::isfinite(degrees)) return Fail(u"That isn't an angle.");
  double d = std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0);
  int64_t rot = std::llround(d * 60000) % 21600000;
  return ChangeDrawing(editor_, drawing, "Rotate picture", [&](Frame& f, std::u16string&) {
    pugi::xml_node extent = Child(f.root, kWp, "extent");
    SetGeometry(f, ClampEmu(AttrInt(extent, "cx", kMinEmu)), ClampEmu(AttrInt(extent, "cy", kMinEmu)), rot);
    return true;
  });
}

Result Pictures::SetCrop(uint32_t drawing, const Crop& crop) {
  if (!ValidCrop(crop)) return Fail(u"That crop would leave nothing of the picture.");
  return ChangeDrawing(editor_, drawing, "Crop picture", [&](Frame& f, std::u16string& why) {
    if (!Child(f.pic, kPic, "blipFill")) {
      why = u"This picture can't be cropped.";
      return false;
    }
    Crop old = CropOf(f);
    if (!ValidCrop(old)) old = Crop{};
    pugi::xml_node extent = Child(f.root, kWp, "extent");
    double cx = static_cast<double>(AttrInt(extent, "cx", kMinEmu)), cy = static_cast<double>(AttrInt(extent, "cy", kMinEmu));
    // The whole picture at its current scale, then the part left showing.
    double fullW = cx / (1 - old.left - old.right), fullH = cy / (1 - old.top - old.bottom);
    int64_t ncx = ClampEmu(std::llround(fullW * (1 - crop.left - crop.right)));
    int64_t ncy = ClampEmu(std::llround(fullH * (1 - crop.top - crop.bottom)));
    SetCropXml(f, crop);
    SetGeometry(f, ncx, ncy, RotationOf(f));
    return true;
  });
}

Result Pictures::SetWrap(uint32_t drawing, Wrap wrap, Emu x, Emu y) {
  const Document& doc = editor_.Doc();
  return ChangeDrawing(editor_, drawing, wrap == Wrap::Inline ? "In line with text" : "Wrap text", [&](Frame& f, std::u16string& why) {
    bool inlineNow = pkg::LocalNameOf(f.root) == "inline";
    if (wrap == Wrap::Inline) return inlineNow || ToInline(f, why);
    if (inlineNow) ToAnchor(f, doc, wrap, x.Value(), y.Value());
    else SetWrapXml(f, wrap);
    return true;
  });
}

Result Pictures::MoveFloating(uint32_t drawing, Emu x, Emu y, std::optional<Pos> anchor) {
  const Document& doc = editor_.Doc();
  if (drawing >= doc.Drawings().size()) return Fail(u"That picture isn't in the document any more.");
  Pos was = doc.Drawings()[drawing].pos;
  bool reanchor = anchor && anchor->para != kNone && was.para != kNone && anchor->para != was.para &&
                  doc.Para(anchor->para).story == doc.Para(was.para).story;
  if (!reanchor) {
    return ChangeDrawing(editor_, drawing, "Move picture", [&](Frame& f, std::u16string& why) {
      if (pkg::LocalNameOf(f.root) != "anchor") {
        why = u"A picture in line with the text moves with it.";
        return false;
      }
      SetPosition(f, x.Value(), y.Value());
      return true;
    });
  }
  // To another paragraph: the picture's run moves there (inserted, then the old one deleted).
  Frame f;
  std::u16string why;
  if (!Load(editor_, drawing, f, why)) return Fail(why);
  if (pkg::LocalNameOf(f.root) != "anchor") return Fail(u"A picture in line with the text moves with it.");
  SetPosition(f, x.Value(), y.Value());
  std::string xml = Serialize(f);
  Drawing handle = HandleFor(doc.Drawings()[drawing], f.root);
  Pos to = *anchor;
  editor_.BeginGroup(u"Move picture");
  Result r = editor_.InsertDrawing(to, xml, handle);
  if (r) {
    Pos old = was;
    if (old.para == to.para && old.offset >= to.offset) ++old.offset;
    Result del = editor_.Delete(old, Pos{old.para, old.offset + 1});
    if (!del) {
      editor_.EndGroup();
      editor_.Undo();
      return del;
    }
  }
  editor_.EndGroup();
  if (r) r.caret = to;
  return r;
}

Result Pictures::MoveInline(uint32_t drawing, Pos to) {
  const Document& doc = editor_.Doc();
  Frame f;
  std::u16string why;
  if (!Load(editor_, drawing, f, why)) return Fail(why);
  if (pkg::LocalNameOf(f.root) != "inline") return Fail(u"Drag a floating picture to place it.");
  Pos was = doc.Drawings()[drawing].pos;
  if (to.para == was.para && (to.offset == was.offset || to.offset == was.offset + 1)) {
    Result r;
    r.caret = was;
    return r;
  }
  std::string xml = Serialize(f);
  Drawing handle = HandleFor(doc.Drawings()[drawing], f.root);
  editor_.BeginGroup(u"Move picture");
  Result r = editor_.InsertDrawing(to, xml, handle);
  Pos final = to;
  if (r) {
    Pos old = was;
    if (old.para == to.para && old.offset >= to.offset) ++old.offset;
    else if (old.para == to.para) --final.offset;  // the old one before it goes
    Result del = editor_.Delete(old, Pos{old.para, old.offset + 1});
    if (!del) {
      editor_.EndGroup();
      editor_.Undo();
      return del;
    }
  }
  editor_.EndGroup();
  if (r) r.caret = final;
  return r;
}

Result Pictures::SetAltText(uint32_t drawing, std::u16string_view text) {
  std::string utf8 = Utf8(text);
  return ChangeDrawing(editor_, drawing, "Alt text", [&](Frame& f, std::u16string&) {
    pugi::xml_node docPr = Child(f.root, kWp, "docPr");
    if (!docPr) docPr = InsertAfterFirst(f.root, f.Wp("docPr"), {{kWp, "extent"}, {kWp, "effectExtent"}, {kWp, "wrapNone"}, {kWp, "wrapSquare"}, {kWp, "wrapTight"}, {kWp, "wrapThrough"}, {kWp, "wrapTopAndBottom"}});
    if (utf8.empty()) {
      docPr.remove_attribute("descr");
    } else {
      pugi::xml_attribute at = docPr.attribute("descr");
      if (!at) at = docPr.append_attribute("descr");
      at = utf8.c_str();
    }
    return true;
  });
}

std::optional<std::pair<Emu, Emu>> Pictures::NaturalSize(uint32_t drawing) const {
  Frame f;
  std::u16string why;
  if (!Load(editor_, drawing, f, why)) return std::nullopt;
  const Document& doc = editor_.Doc();
  std::string rid = BlipId(f);
  std::vector<uint8_t> bytes;
  if (rid.empty() || !doc.ReadTarget(doc.Drawings()[drawing].part, rid, bytes).Ok()) return std::nullopt;
  std::shared_ptr<draw::Image> image = draw::Image::Open(std::move(bytes));
  if (!image) return std::nullopt;
  draw::Size n = image->NaturalSize();
  if (!(n.width > 0 && n.height > 0 && std::isfinite(n.width) && std::isfinite(n.height))) return std::nullopt;
  return std::make_pair(Emu(ClampEmu(std::llround(n.width * kEmuPerPoint))), Emu(ClampEmu(std::llround(n.height * kEmuPerPoint))));
}

Result Pictures::ResetSize(uint32_t drawing) {
  std::optional<std::pair<Emu, Emu>> natural = NaturalSize(drawing);
  if (!natural) {
    Frame f;
    std::u16string why;
    if (!Load(editor_, drawing, f, why)) return Fail(why);
    return Fail(u"This picture's own size can't be read.");
  }
  return ChangeDrawing(editor_, drawing, "Reset picture", [&](Frame& f, std::u16string&) {
    SetCropXml(f, Crop{});
    SetGeometry(f, natural->first.Value(), natural->second.Value(), 0);
    return true;
  });
}

// ---------- Unused media ----------

UnusedMedia FindUnusedMedia(const Document& doc, const std::vector<std::pair<uint32_t, const std::string*>>& written) {
  UnusedMedia out;
  const pkg::Package& package = doc.Package();
  for (const auto& [index, xml] : written) {
    if (index >= doc.Parts().size() || !xml) continue;
    const pkg::RelationshipSet* rels = package.RelationshipsOf(doc.Parts()[index].name);
    if (!rels) continue;
    for (const pkg::Relationship& r : rels->items) {
      if (r.external || r.type != kImageRel) continue;
      // Any mention of the id keeps it: an attribute, kept XML, a field.
      if (xml->find("\"" + r.id + "\"") != std::string::npos || xml->find("'" + r.id + "'") != std::string::npos) continue;
      out.relationships.emplace_back(rels->source, r.id);
    }
  }
  // A part goes when every relationship to it does.
  for (const auto& [source, id] : out.relationships) {
    const pkg::RelationshipSet* rels = package.RelationshipsOf(source);
    const pkg::Relationship* r = rels ? rels->Find(id) : nullptr;
    if (!r || r->targetPart.empty()) continue;
    std::string target = Lower(r->targetPart);
    if (std::find(out.parts.begin(), out.parts.end(), r->targetPart) != out.parts.end()) continue;
    bool used = false;
    for (const pkg::RelationshipSet& set : package.Relationships())
      for (const pkg::Relationship& other : set.items) {
        if (other.external || Lower(other.targetPart) != target) continue;
        bool going = std::find(out.relationships.begin(), out.relationships.end(), std::make_pair(set.source, other.id)) !=
                     out.relationships.end();
        if (!going) used = true;
      }
    if (!used) out.parts.push_back(r->targetPart);
  }
  return out;
}

}  // namespace fw::doc::edit
