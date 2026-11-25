// The presentation tab's Transitions and Animations (P3, p3-anim; plan §8.2): PowerPoint 2021's two
// ribbon tabs, the Animation Pane and Preview in the editor. Every change goes through the edit
// layer's TimingOps (src/slide/edit/TimingOps.h), one undo step each; what Featherweight can't change
// (Morph, motion paths, other effects) is listed and kept.

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "Commands.h"
#include "core/Strings.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/TimingOps.h"
#include "slide/view/ObjectTool.h"
#include "slide/view/PresentationAnim.h"
#include "slide/view/PresentationView.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/PanelButton.h"
#include "ui/RibbonModel.h"
#include "ui/SidePanel.h"
#include "ui/Theme.h"

using namespace gfx;
namespace ed = fw::slide::edit;

namespace fw::slide::view {

namespace {

constexpr ed::TransitionEffect kTransitionGallery[] = {
    ed::TransitionEffect::None,  ed::TransitionEffect::Cut,     ed::TransitionEffect::Fade,
    ed::TransitionEffect::Push,  ed::TransitionEffect::Wipe,    ed::TransitionEffect::Split,
    ed::TransitionEffect::Uncover, ed::TransitionEffect::Cover, ed::TransitionEffect::Zoom};
// The Animations gallery after None: entrances, emphasis, exits, in PowerPoint's order.
constexpr ed::Anim kAnimGallery[] = {ed::Anim::Appear,    ed::Anim::Fade,       ed::Anim::FlyIn,   ed::Anim::FloatIn,
                                     ed::Anim::Split,     ed::Anim::Wipe,       ed::Anim::Zoom,    ed::Anim::Spin,
                                     ed::Anim::GrowShrink, ed::Anim::Disappear, ed::Anim::FadeOut, ed::Anim::FlyOut,
                                     ed::Anim::FloatOut,  ed::Anim::SplitOut,   ed::Anim::WipeOut, ed::Anim::ZoomOut};

constexpr wchar_t kPlay = 0xE768, kStar = 0xE735, kOptions = 0xE8CB, kPane = 0xE8A0, kClock = 0xE823, kMouse = 0xE962,
                  kApplyAll = 0xE8B3;

std::wstring W(std::u16string_view s) { return std::wstring(s.begin(), s.end()); }

std::wstring Seconds(double ms) {
  wchar_t buf[32];
  swprintf_s(buf, L"%.2f s", ms / 1000);
  return buf;
}

// A gallery name that tells the exits from the entrances of the same name.
std::wstring AnimLabel(ed::Anim a) {
  std::wstring n = W(ed::AnimName(a));
  if (ed::ClassOf(a) == ed::AnimClass::Exit && (a == ed::Anim::FadeOut || a == ed::Anim::SplitOut || a == ed::Anim::WipeOut || a == ed::Anim::ZoomOut))
    n += L" (exit)";
  return n;
}

D2D1_COLOR_F ClassColor(ed::AnimClass c) {
  switch (c) {
    case ed::AnimClass::Entrance: return Rgb(0x3A9D23);
    case ed::AnimClass::Emphasis: return Rgb(0xE3A21A);
    case ed::AnimClass::Exit: return Rgb(0xD13438);
    default: return Rgb(0x4A7BD0);
  }
}

int GalleryIndex(ed::Anim a) {
  for (size_t i = 0; i < std::size(kAnimGallery); ++i)
    if (kAnimGallery[i] == a) return static_cast<int>(i) + 1;
  return -1;
}

// A choice of either gallery: its picture (a star in its class's color, or a slide) over its name.
void DrawChoice(Canvas& c, const D2D1_RECT_F& r, const std::wstring& name, wchar_t glyph, D2D1_COLOR_F color, float ui) {
  const D2D1_RECT_F pic = RectF(r.left, r.top + 4 * ui, r.right, r.bottom - 18 * ui);
  c.Icon(glyph, pic, color, 22 * ui);
  c.Text(name, RectF(r.left + 2 * ui, r.bottom - 18 * ui, r.right - 2 * ui, r.bottom - 2 * ui), theme::P().text, 11 * ui,
         DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
}

const Shape* FindShapeIn(const std::vector<Shape>& shapes, uint32_t id) {
  for (const Shape& s : shapes) {
    if (s.id == id) return &s;
    if (const Shape* in = FindShapeIn(s.children, id)) return in;
  }
  return nullptr;
}

// The shape's paragraphs with text in them.
int TextParagraphs(const Shape* sh) {
  int n = 0;
  if (sh && sh->textNode)
    for (pugi::xml_node p = sh->textNode.first_child(); p; p = p.next_sibling())
      if (std::string_view(p.name()).ends_with(":p") && p.find_node([](pugi::xml_node x) { return std::string_view(x.name()).ends_with(":t") && *x.text().get(); }))
        ++n;
  return n;
}

// Effect Options > Sequence: every target a common effect on a shape with text.
bool SequenceApplies(const Slide* slide, const std::vector<ed::AnimEffect>& seq, const std::vector<size_t>& targets) {
  if (!slide || targets.empty()) return false;
  for (size_t k : targets)
    if (k >= seq.size() || !seq[k].editable || TextParagraphs(FindShapeIn(slide->shapes, seq[k].shape)) == 0) return false;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Preview (filled in with the show's drawing)

namespace {

// A context for drawing off-screen on the same device as `dc`, so its bitmaps draw on `dc`.
ID2D1DeviceContext* PreviewContext(ID2D1DeviceContext* dc) {
  static Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx;
  static ID2D1Device* of = nullptr;
  Microsoft::WRL::ComPtr<ID2D1Device> device;
  dc->GetDevice(&device);
  if (!device) return nullptr;
  if (!ctx || of != device.Get()) {
    ctx.Reset();
    of = nullptr;
    if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx))) return nullptr;
    of = device.Get();
  }
  return ctx.Get();
}

constexpr double kHoldMs = 600;  // the last frame of an animation preview stays this long

}  // namespace

void PresentationView::StartPreview(bool transition, size_t fromEffect) {
  StopPreview();
  edit::Editor* e = SlideEditor();
  Presentation* model = deck_ ? deck_->Model() : nullptr;
  if (!Ready() || !SlideCount() || !model || !e) return;
  const Slide* slide = model->GetSlide(current_);
  if (!slide) return;
  auto p = std::make_unique<AnimPreview>();
  p->transition = transition;
  p->slide = current_;
  if (transition) {
    p->trans = show::ParseTransition(slide->transition);
    if (p->trans.kind == show::TransitionKind::None || p->trans.durationMs <= 0) return;
  } else {
    p->timing = show::ParseTiming(slide->timing);
    if (p->timing.main.steps.empty()) return;
    // The click the effect plays on: steps are the sequence's click groups.
    const std::vector<ed::AnimEffect> seq = AnimNow().seq;
    // (A sequence that starts with the slide has that group as step 0; else the first click's is.)
    size_t clicks = 0;
    for (size_t i = 0; i < seq.size() && i <= fromEffect; ++i)
      if (seq[i].trigger == ed::AnimTrigger::OnClick && i > 0) ++clicks;
    p->fromStep = std::min(clicks, p->timing.main.steps.size() - 1);
    p->renderer = show::MakeRenderer(*model);  // fresh: the slides as they are now
  }
  p->start = anim::Now();
  preview_ = std::move(p);
  host_.ViewInvalidate();
}

void PresentationView::StopPreview() {
  if (!preview_) return;
  preview_.reset();
  slideBitmap_.Reset();
  host_.ViewInvalidate();
}

bool PresentationView::DrawPreview(Canvas& /*c*/, ID2D1DeviceContext* dc) {
  AnimPreview& p = *preview_;
  if (p.slide != current_) {
    preview_.reset();
    return false;
  }
  const D2D1_RECT_F rc = SlideRect();
  const UINT w = static_cast<UINT>(std::lround(Width(rc))), h = static_cast<UINT>(std::lround(Height(rc)));
  ID2D1DeviceContext* ctx = PreviewContext(dc);
  if (!ctx || !w || !h || w > 4096 || h > 4096) {
    preview_.reset();
    return false;
  }
  const double now = p.fixedAt >= 0 ? p.start + p.fixedAt : anim::Now();
  if (!p.prepared || p.w != w || p.h != h) {
    p.w = w;
    p.h = h;
    p.prepared = true;
    if (p.transition) {
      p.to = RenderBitmap(dc, current_, w, h);
      p.from = current_ > 0 ? RenderBitmap(dc, current_ - 1, w, h) : nullptr;
      const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
      p.frame.Reset();
      if (!p.to || FAILED(ctx->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &p.frame))) {
        preview_.reset();
        return false;
      }
    } else {
      if (!p.player.Prepare(ctx, *p.renderer, current_, p.timing, w, h, p.start)) {
        preview_.reset();
        return false;
      }
      // The steps before the one to play from, done at once; that one starts now.
      show::Animator& a = p.player.Anim();
      if (p.fromStep > 0) {
        a.Finish();
        while (static_cast<size_t>(a.StepIndex()) < p.fromStep && a.HasNextStep()) {
          a.Click(p.start);
          a.Finish();
        }
      }
      if (!a.Running(p.start) && a.HasNextStep()) a.Click(p.start);
    }
  }
  bool done = false;
  if (p.transition) {
    const double t = std::clamp((now - p.start) / std::max(1.0, p.trans.durationMs), 0.0, 1.0);
    ctx->SetTarget(p.frame.Get());
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    show::DrawTransitionFrame(ctx, p.trans, p.from.Get(), p.to.Get(), static_cast<float>(w), static_cast<float>(h), t);
    ctx->EndDraw();
    ctx->SetTarget(nullptr);
    dc->DrawBitmap(p.frame.Get(), rc, 1, D2D1_INTERPOLATION_MODE_LINEAR);
    done = t >= 1;
  } else {
    // Each step plays when the one before it ends, as if clicked: PowerPoint's Preview.
    show::Animator& a = p.player.Anim();
    a.Update(now);
    if (!a.Running(now)) {
      if (a.HasNextStep()) {
        a.Click(now);
      } else if (p.settled < 0) {
        p.settled = now;
      }
    }
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    dc->PushAxisAlignedClip(rc, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(rc.left, rc.top) * old);
    p.player.Draw(dc);
    dc->SetTransform(old);
    dc->PopAxisAlignedClip();
    done = p.settled >= 0 && now - p.settled > kHoldMs;
  }
  if (done && p.fixedAt < 0) {
    preview_.reset();
    host_.ViewInvalidate();
    return true;
  }
  anim::KeepAlive(now + 100);
  host_.ViewInvalidate();
  return true;
}

// ---------------------------------------------------------------------------------------------
// The Animation Pane

PresentationView::AnimationPane::AnimationPane(PresentationView& v) : v_(v) {
  auto button = [](PanelButton& b, const wchar_t* text, std::function<void()> fn) {
    b.text = text;
    b.textSize = 12;
    b.onInvoke = std::move(fn);
  };
  button(play_, L"Play", [this] { v_.StartPreview(false, picked_.empty() ? 0 : picked_.front()); });
  button(up_, L"Move up", [this] { v_.AnimCommand(CmdAnimEarlier); });
  button(down_, L"Move down", [this] { v_.AnimCommand(CmdAnimLater); });
  button(remove_, L"Remove", [this] { v_.AnimCommand(CmdAnimRemove); });
}

// The slide's sequence again, when the deck or the slide changed.
void PresentationView::AnimationPane::Sync() {
  edit::Editor* e = v_.SlideEditor();
  const uint64_t state = e ? e->StateId() : 0;
  if (e && state == state_ && v_.current_ == slide_) return;
  state_ = state;
  slide_ = v_.current_;
  seq_ = e && v_.SlideCount() ? ed::MainSequence(*e, slide_, &why_) : std::vector<ed::AnimEffect>{};
  names_.clear();
  const Presentation* deck = v_.deck_ ? v_.deck_->Model() : nullptr;
  const Slide* slide = deck && slide_ < deck->SlideCount() ? const_cast<Presentation*>(deck)->GetSlide(slide_) : nullptr;
  for (const ed::AnimEffect& a : seq_) {
    const Shape* sh = slide ? FindShapeIn(slide->shapes, a.shape) : nullptr;
    std::wstring name = sh && !sh->name.empty() ? fw::Utf8ToWide(sh->name) : a.shape ? L"Shape " + std::to_wstring(a.shape) : L"Sound";
    if (a.paragraph >= 0) name += L", paragraph " + std::to_wstring(a.paragraph + 1);
    names_.push_back((a.anim == ed::Anim::Other ? std::wstring(L"Custom") : W(ed::AnimName(a.anim))) + L": " + name);
  }
}

std::vector<Control*> PresentationView::AnimationPane::Layout(const D2D1_RECT_F& body, float ui) {
  Sync();
  ui_ = ui;
  const float left = body.left + 12 * ui, right = body.right - 12 * ui, rowH = 28 * ui, gap = 6 * ui;
  const float w = (right - left - 3 * gap) / 4;
  PanelButton* row[] = {&play_, &up_, &down_, &remove_};
  for (int i = 0; i < 4; ++i) row[i]->rc = RectF(left + i * (w + gap), body.top + 8 * ui, left + i * (w + gap) + w, body.top + 8 * ui + rowH);
  list_ = RectF(left, body.top + 8 * ui + rowH + 10 * ui, right, body.bottom - 8 * ui);
  return {&play_, &up_, &down_, &remove_};
}

void PresentationView::AnimationPane::Draw(Canvas& c) {
  Sync();
  // What's picked follows the slide's selection; the buttons follow what's picked.
  picked_ = v_.AnimTargets();
  play_.enabled = !seq_.empty();
  up_.enabled = !picked_.empty() && picked_.front() > 0 && why_.empty();
  down_.enabled = !picked_.empty() && picked_.back() + 1 < seq_.size() && why_.empty();
  remove_.enabled = !picked_.empty() && why_.empty();
  const Palette& p = theme::P();
  const float rowH = 26 * ui_;
  c.PushClip(list_);
  if (seq_.empty()) {
    c.TextWrapped(!why_.empty() ? W(why_) : std::wstring(L"No animations on this slide. Select a shape and pick an animation on the Animations tab."),
                  RectF(list_.left, list_.top, list_.right, list_.top + 60 * ui_), p.textSecondary, 12 * ui_, 3);
  }
  int click = 0;
  for (size_t i = 0; i < seq_.size(); ++i) {
    const ed::AnimEffect& a = seq_[i];
    if (a.trigger == ed::AnimTrigger::OnClick) ++click;
    const float y = list_.top + i * rowH - scroll_;
    if (y + rowH < list_.top || y > list_.bottom) continue;
    const D2D1_RECT_F row = RectF(list_.left, y, list_.right, y + rowH);
    const bool sel = std::find(picked_.begin(), picked_.end(), i) != picked_.end();
    if (sel) c.FillRound(row, 3 * ui_, Alpha(p.accent, focus_ ? 0.30f : 0.18f));
    float x = row.left + 4 * ui_;
    // The click it plays on (0: with the slide), as PowerPoint numbers them.
    if (a.trigger == ed::AnimTrigger::OnClick || i == 0)
      c.Text(std::to_wstring(a.trigger == ed::AnimTrigger::OnClick ? click : 0), RectF(x, y, x + 18 * ui_, y + rowH), p.textSecondary,
             11 * ui_, DWRITE_FONT_WEIGHT_REGULAR, Align::Center);
    x += 20 * ui_;
    const wchar_t trig = a.trigger == ed::AnimTrigger::OnClick ? kMouse : a.trigger == ed::AnimTrigger::AfterPrevious ? kClock : 0;
    if (trig) c.Icon(trig, RectF(x, y, x + 16 * ui_, y + rowH), p.textSecondary, 12 * ui_);
    x += 18 * ui_;
    c.Icon(kStar, RectF(x, y, x + 16 * ui_, y + rowH), ClassColor(a.cls), 13 * ui_);
    x += 20 * ui_;
    c.Text(names_[i], RectF(x, y, row.right - 4 * ui_, y + rowH), a.editable ? p.text : p.textSecondary, 12 * ui_);
  }
  if (dropAt_ != SIZE_MAX) {  // where a drag would put the picked effects
    const float y = list_.top + dropAt_ * rowH - scroll_;
    c.FillRound(RectF(list_.left, y - 1 * ui_, list_.right, y + 1 * ui_), 1 * ui_, p.accent);
  }
  c.PopClip();
}

void PresentationView::AnimationPane::SetFocus(bool has, bool /*byKeyboard*/) { focus_ = has; }

bool PresentationView::AnimationPane::OnMouseDown(float x, float y, bool /*dbl*/) {
  if (x < list_.left || x > list_.right || y < list_.top || y > list_.bottom) return false;
  const size_t i = static_cast<size_t>((y - list_.top + scroll_) / (26 * ui_));
  if (i >= seq_.size()) return false;
  std::vector<size_t> pick;
  if (GetKeyState(VK_SHIFT) < 0 && !picked_.empty()) {
    for (size_t k = std::min(anchor_, i); k <= std::max(anchor_, i); ++k) pick.push_back(k);
  } else if (GetKeyState(VK_CONTROL) < 0) {
    pick = picked_;
    auto it = std::find(pick.begin(), pick.end(), i);
    if (it != pick.end()) pick.erase(it);
    else pick.push_back(i);
    anchor_ = i;
  } else {
    pick = {i};
    anchor_ = i;
  }
  v_.PickAnims(pick);
  pressed_ = why_.empty();
  pressY_ = y;
  dropAt_ = SIZE_MAX;
  return true;
}

bool PresentationView::AnimationPane::OnMouseMove(float /*x*/, float y) {
  if (pressed_ && GetKeyState(VK_LBUTTON) >= 0) {  // released where the pane didn't hear it
    pressed_ = false;
    const bool shown = dropAt_ != SIZE_MAX;
    dropAt_ = SIZE_MAX;
    return shown;
  }
  if (!pressed_ || (dropAt_ == SIZE_MAX && std::fabs(y - pressY_) < 5 * ui_)) return false;
  const float at = std::round((y - list_.top + scroll_) / (26 * ui_));
  dropAt_ = static_cast<size_t>(std::clamp(at, 0.f, static_cast<float>(seq_.size())));
  return true;
}

void PresentationView::AnimationPane::OnMouseUp(float /*x*/, float /*y*/) {
  const size_t drop = dropAt_;
  pressed_ = false;
  dropAt_ = SIZE_MAX;
  edit::Editor* e = v_.SlideEditor();
  const std::vector<size_t> t = v_.AnimTargets();
  if (drop == SIZE_MAX || !e || t.empty()) return;
  // `to` counts the effects that stay: the moved ones above the drop place don't.
  size_t to = drop;
  for (size_t k : t)
    if (k < drop) --to;
  if (to == t.front() && t.back() - t.front() + 1 == t.size()) return;  // dropped where they are
  if (v_.Done(ed::MoveAnimations(*e, v_.current_, t, to))) {
    std::vector<size_t> moved;
    for (size_t k = 0; k < t.size(); ++k) moved.push_back(to + k);
    v_.PickAnims(moved);
  }
  v_.AnimChanged();
}

bool PresentationView::AnimationPane::OnWheel(int delta, float /*x*/, float /*y*/) {
  const float content = seq_.size() * 26 * ui_, room = list_.bottom - list_.top;
  scroll_ = std::clamp(scroll_ - delta / 120.f * 3 * 26 * ui_, 0.f, std::max(0.f, content - room));
  return true;
}

bool PresentationView::AnimationPane::OnKeyDown(UINT vk, bool /*ctrl*/, bool shift) {
  if (seq_.empty()) return false;
  if (vk == VK_DELETE) return v_.AnimCommand(CmdAnimRemove);
  if (vk != VK_UP && vk != VK_DOWN) return false;
  size_t at = picked_.empty() ? 0 : vk == VK_UP ? picked_.front() : picked_.back();
  if (!picked_.empty()) at = vk == VK_UP ? (at ? at - 1 : 0) : std::min(at + 1, seq_.size() - 1);
  if (shift && !picked_.empty()) {
    std::vector<size_t> pick;
    for (size_t k = std::min(anchor_, at); k <= std::max(anchor_, at); ++k) pick.push_back(k);
    v_.PickAnims(pick);
  } else {
    anchor_ = at;
    v_.PickAnims({at});
  }
  return true;
}

std::wstring PresentationView::AnimationPane::TooltipAt(float x, float y) {
  if (x < list_.left || x > list_.right || y < list_.top || y > list_.bottom) return {};
  const size_t i = static_cast<size_t>((y - list_.top + scroll_) / (26 * ui_));
  if (i >= seq_.size()) return {};
  const ed::AnimEffect& a = seq_[i];
  const wchar_t* start = a.trigger == ed::AnimTrigger::OnClick ? L"On Click" : a.trigger == ed::AnimTrigger::WithPrevious ? L"With Previous" : L"After Previous";
  std::wstring t = names_[i] + L"\n" + start + L", " + Seconds(a.durationMs);
  if (a.delayMs > 0) t += L", after a delay of " + Seconds(a.delayMs);
  if (!a.editable) t += L"\nFeatherweight keeps this effect as it is; it can be moved, retimed or removed.";
  return t;
}


void PresentationView::AnimChanged() {
  if (animPane_ && host_.ViewShownPanel() == animPane_.get()) host_.ViewInvalidate();
}

// ---------------------------------------------------------------------------------------------
// What the controls act on

const PresentationView::AnimCache& PresentationView::AnimNow() {
  if (!animCache_) animCache_ = std::make_unique<AnimCache>();
  AnimCache& c = *animCache_;
  edit::Editor* e = SlideEditor();
  const uint64_t state = e ? e->StateId() : 0;
  if (c.editor != e || c.state != state || c.edits != animEdits_ || c.slide != current_) {
    const bool any = e && current_ < SlideCount();
    c.seq = any ? ed::MainSequence(*e, current_) : std::vector<ed::AnimEffect>{};
    c.transition = any ? ed::GetTransition(*e, current_) : ed::TransitionInfo{};
    c.editor = e;
    c.state = state;
    c.edits = animEdits_;
    c.slide = current_;
  }
  return c;
}

std::vector<size_t> PresentationView::AnimTargets() {
  edit::Editor* e = SlideEditor();
  if (!e || !SlideCount()) return {};
  const std::vector<ed::AnimEffect> seq = AnimNow().seq;
  const std::vector<uint32_t>& sel = Objects().Selection();
  std::vector<size_t> out;
  if (animPickedSlide_ == current_ && animPickedShapes_ == sel) {
    for (size_t i : animPicked_)
      if (i < seq.size()) out.push_back(i);
    if (!out.empty()) return out;
  }
  for (size_t i = 0; i < seq.size(); ++i)
    if (std::find(sel.begin(), sel.end(), seq[i].shape) != sel.end()) out.push_back(i);
  return out;
}

void PresentationView::PickAnims(const std::vector<size_t>& effects) {
  edit::Editor* e = SlideEditor();
  if (!e) return;
  std::vector<size_t> pick(effects);
  std::sort(pick.begin(), pick.end());
  pick.erase(std::unique(pick.begin(), pick.end()), pick.end());
  // The shapes they animate are selected on the slide too, as PowerPoint does.
  const std::vector<ed::AnimEffect> seq = AnimNow().seq;
  std::vector<uint32_t> shapes;
  for (size_t i : pick)
    if (i < seq.size() && seq[i].shape && std::find(shapes.begin(), shapes.end(), seq[i].shape) == shapes.end())
      shapes.push_back(seq[i].shape);
  Objects().Select(shapes);
  animPicked_ = pick;
  animPickedSlide_ = current_;
  animPickedShapes_ = Objects().Selection();
  host_.ViewInvalidate();
  host_.ViewStateChanged();
}

// ---------------------------------------------------------------------------------------------
// Changes

void PresentationView::ApplyAnim(int index, bool add) {
  edit::Editor* e = SlideEditor();
  if (!e || !SlideCount() || !CanEditSlides()) return;
  const std::vector<size_t> targets = AnimTargets();
  const std::vector<uint32_t> sel = Objects().Selection();
  const std::vector<ed::AnimEffect> seq = AnimNow().seq;
  if (index < 0 || index > static_cast<int>(std::size(kAnimGallery))) return;
  const Presentation* deck = deck_->Model();
  const Slide* slide = deck ? const_cast<Presentation*>(deck)->GetSlide(current_) : nullptr;
  auto spec = [&](uint32_t id, ed::Anim a) {
    ed::AnimSpec s;
    s.anim = a;
    // A body placeholder with several paragraphs builds a paragraph at a time, as PowerPoint's default.
    const Shape* sh = slide ? FindShapeIn(slide->shapes, id) : nullptr;
    if (sh && (sh->placeholder.type == PlaceholderType::Body || sh->placeholder.type == PlaceholderType::Object))
      s.byParagraph = TextParagraphs(sh) > 1;
    return s;
  };
  edit::EditGroup group(*e, add ? u"Add Animation" : index == 0 ? u"Remove Animation" : u"Animation");
  bool ok = true;
  if (index == 0) {
    if (!targets.empty()) ok = Done(ed::RemoveAnimations(*e, current_, targets));
  } else {
    const ed::Anim a = kAnimGallery[index - 1];
    if (!add && !targets.empty()) ok = Done(ed::ChangeAnimation(*e, current_, targets, a));
    for (uint32_t id : sel) {
      if (!ok) break;
      const bool has = std::any_of(seq.begin(), seq.end(), [&](const ed::AnimEffect& x) { return x.shape == id; });
      if (add || !has) ok = Done(ed::AddAnimation(*e, current_, id, spec(id, a)));
    }
  }
  group.done = ok;
  animPicked_.clear();
  AnimChanged();
}

// ---------------------------------------------------------------------------------------------
// The tabs

void PresentationView::BuildAnimTabs(RibbonModel& m) {
  edit::Editor* e = SlideEditor();
  const bool ready = Ready() && SlideCount() > 0 && e;
  const ed::TransitionInfo tr = ready ? AnimNow().transition : ed::TransitionInfo{};
  auto tab = [&](const wchar_t* name, const wchar_t* keyTip) {
    RibbonTab t;
    t.name = name;
    t.keyTip = keyTip;
    m.tabs.push_back(std::move(t));
  };
  auto group = [&](const wchar_t* name, int rows = 3, int shrinkFirst = 0) {
    RibbonGroup g;
    g.name = name;
    g.rows = rows;
    g.shrinkFirst = shrinkFirst;
    m.tabs.back().groups.push_back(std::move(g));
  };
  auto add = [&](int cmd, wchar_t glyph, RibbonSize size, const wchar_t* label, const wchar_t* keyTip) -> RibbonItem& {
    RibbonItem it;
    const CommandState q = QueryCommand(cmd);
    it.tool.cmd = cmd;
    it.tool.glyph = glyph;
    it.tool.enabled = q.enabled;
    it.tool.checked = q.checked;
    it.size = size;
    it.smallest = size;
    it.label = label;
    it.keyTip = keyTip;
    std::vector<RibbonItem>& items = m.tabs.back().groups.back().items;
    items.push_back(std::move(it));
    return items.back();
  };
  auto check = [&](int cmd, const wchar_t* label, const wchar_t* keyTip) -> RibbonItem& {
    RibbonItem& it = add(cmd, 0, RibbonSize::Medium, label, keyTip);
    it.tool.toggle = true;
    return it;
  };
  auto spin = [&](int cmd, const wchar_t* label, const wchar_t* keyTip, double seconds, double max, std::function<void(double)> set) -> RibbonItem& {
    RibbonItem& it = add(cmd, 0, RibbonSize::Medium, label, keyTip);
    it.tool.kind = ToolKind::SpinBox;
    it.tool.width = 90;
    it.tool.value = seconds;
    it.tool.minValue = 0;
    it.tool.maxValue = max;
    it.tool.step = 0.25;
    it.tool.decimals = 2;
    it.tool.unit = L" s";
    it.tool.tooltip = label;
    it.tool.onChange = [set = std::move(set)](const ToolValue& v) { set(v.number); };
    return it;
  };
  auto menuOf = [&](int first, size_t count) {
    std::vector<int> out;
    for (size_t i = 0; i < count; ++i) out.push_back(first + static_cast<int>(i));
    return out;
  };

  // ---- Transitions
  tab(L"Transitions", L"K");
  group(L"Preview");
  add(CmdTransitionPreview, kPlay, RibbonSize::Large, L"Preview", L"P");
  group(L"Transition to This Slide", 3, 1);
  {
    RibbonItem& g = add(CmdTransitionStyles, icon::Slideshow, RibbonSize::Large, L"Transitions", L"TS");
    for (size_t i = 0; i < std::size(kTransitionGallery); ++i) {
      g.tool.choices.push_back(W(ed::TransitionName(kTransitionGallery[i])));
      if (kTransitionGallery[i] == tr.effect) g.tool.choice = static_cast<int>(i);
    }
    g.tool.enabled = ready;
    g.tool.onChange = [this](const ToolValue& v) {
      if (v.index < 0 || v.index >= static_cast<int>(std::size(kTransitionGallery))) return;
      edit::Editor* ed2 = SlideEditor();
      if (!ed2 || !CanEditSlides()) return;
      ed::TransitionPatch p;
      p.effect = kTransitionGallery[v.index];
      Done(ed::SetTransition(*ed2, SelectedSlides(), p));
    };
    RibbonGallery gal;
    gal.choiceWidth = 66;
    gal.choiceHeight = 58;
    gal.columns = static_cast<int>(std::size(kTransitionGallery));
    gal.minColumns = 3;
    gal.drawChoice = [](Canvas& c, const D2D1_RECT_F& r, int i, float ui) {
      if (i < 0 || i >= static_cast<int>(std::size(kTransitionGallery))) return;
      DrawChoice(c, r, W(ed::TransitionName(kTransitionGallery[i])), i == 0 ? icon::Cancel : icon::Slideshow, theme::P().accentText, ui);
    };
    g.gallery = std::move(gal);
  }
  add(CmdTransitionOptionsMenu, kOptions, RibbonSize::Large, L"Effect\nOptions", L"EO").menu =
      menuOf(CmdTransitionOption, ed::TransitionOptions(tr.effect).size());
  m.tabs.back().groups.back().items.back().tool.dropdown = true;
  group(L"Timing");
  spin(CmdTransitionDuration, L"Duration", L"DU", tr.durationMs / 1000, 59, [this](double s) {
    edit::Editor* ed2 = SlideEditor();
    if (!ed2 || !CanEditSlides()) return;
    ed::TransitionPatch p;
    p.durationMs = std::clamp(s, 0.01, 59.0) * 1000;
    Done(ed::SetTransition(*ed2, SelectedSlides(), p));
  });
  add(CmdTransitionApplyAll, kApplyAll, RibbonSize::Medium, L"Apply To All", L"AL");
  check(CmdTransitionOnClick, L"On Mouse Click", L"MC").newColumn = true;
  check(CmdTransitionAfter, L"After:", L"AF");
  RibbonItem& after = spin(CmdTransitionAfterTime, L"After (seconds)", L"AT", std::max(0.0, tr.advanceAfterMs) / 1000, 86399, [this](double s) {
    edit::Editor* ed2 = SlideEditor();
    if (!ed2 || !CanEditSlides()) return;
    ed::TransitionPatch p;
    p.advanceAfterMs = std::max(0.0, s) * 1000;
    Done(ed::SetTransition(*ed2, SelectedSlides(), p));
  });
  after.sameRow = true;
  after.size = after.smallest = RibbonSize::Small;  // the box alone, beside its check box

  // ---- Animations
  const std::vector<size_t> targets = ready ? AnimTargets() : std::vector<size_t>{};
  const std::vector<ed::AnimEffect> seq = ready ? AnimNow().seq : std::vector<ed::AnimEffect>{};
  // What the selected effects have in common (none: the gallery shows None, the boxes are empty).
  int common = -1;
  ed::AnimTrigger trigger = ed::AnimTrigger::OnClick;
  double duration = 0, delay = 0;
  for (size_t k = 0; k < targets.size(); ++k) {
    const ed::AnimEffect& a = seq[targets[k]];
    const int g = GalleryIndex(a.anim);
    if (k == 0) {
      common = g;
      trigger = a.trigger;
      duration = a.durationMs / 1000;
      delay = a.delayMs / 1000;
    } else if (g != common) {
      common = -1;
    }
  }
  const bool shapes = ready && !Objects().Selection().empty();
  tab(L"Animations", L"A");
  group(L"Preview");
  add(CmdAnimPreview, kPlay, RibbonSize::Large, L"Preview", L"P");
  group(L"Animation", 3, 1);
  auto animChoices = [&](RibbonItem& g, bool none) {
    if (none) g.tool.choices.push_back(L"None");
    for (ed::Anim a : kAnimGallery) g.tool.choices.push_back(AnimLabel(a));
    RibbonGallery gal;
    gal.choiceWidth = 66;
    gal.choiceHeight = 58;
    gal.drawChoice = [none](Canvas& c, const D2D1_RECT_F& r, int i, float ui) {
      if (none && i == 0) {
        DrawChoice(c, r, L"None", icon::Cancel, theme::P().textSecondary, ui);
        return;
      }
      const int k = i - (none ? 1 : 0);
      if (k < 0 || k >= static_cast<int>(std::size(kAnimGallery))) return;
      DrawChoice(c, r, AnimLabel(kAnimGallery[k]), kStar, ClassColor(ed::ClassOf(kAnimGallery[k])), ui);
    };
    g.gallery = std::move(gal);
  };
  {
    RibbonItem& g = add(CmdAnimStyles, kStar, RibbonSize::Large, L"Animation\nStyles", L"AS");
    animChoices(g, true);
    g.gallery->columns = 7;
    g.gallery->minColumns = 3;
    g.gallery->popupColumns = 6;
    g.tool.enabled = shapes || !targets.empty();
    g.tool.choice = targets.empty() ? (shapes ? 0 : -1) : common;
    g.tool.onChange = [this](const ToolValue& v) { ApplyAnim(v.index, false); };
  }
  {
    const ed::Anim a = common > 0 ? kAnimGallery[common - 1] : ed::Anim::Other;
    RibbonItem& it = add(CmdAnimOptionsMenu, kOptions, RibbonSize::Large, L"Effect\nOptions", L"EM");
    it.tool.dropdown = true;
    it.menu = menuOf(CmdAnimOption, ed::AnimOptions(a).size());
    if (!it.menu.empty()) it.menu.push_back(0);
    it.menu.push_back(CmdAnimAsOneObject);
    it.menu.push_back(CmdAnimByParagraph);
  }
  group(L"Advanced Animation");
  {
    RibbonItem& g = add(CmdAnimAddMenu, icon::Add, RibbonSize::Large, L"Add\nAnimation", L"AA");
    animChoices(g, false);
    g.gallery->columns = 0;
    g.gallery->popupColumns = 6;
    g.tool.dropdown = true;
    g.tool.enabled = shapes;
    g.tool.onChange = [this](const ToolValue& v) { ApplyAnim(v.index + 1, true); };
  }
  add(CmdAnimPane, kPane, RibbonSize::Medium, L"Animation Pane", L"C").tool.toggle = true;
  group(L"Timing");
  {
    RibbonItem& it = add(CmdAnimStart, 0, RibbonSize::Medium, L"Start", L"ST");
    it.tool.kind = ToolKind::Dropdown;
    it.tool.width = 130;
    it.tool.choices = {L"On Click", L"With Previous", L"After Previous"};
    it.tool.choice = targets.empty() ? -1 : static_cast<int>(trigger);
    it.tool.tooltip = L"Start";
    it.tool.onChange = [this](const ToolValue& v) {
      edit::Editor* ed2 = SlideEditor();
      const std::vector<size_t> t = AnimTargets();
      if (!ed2 || t.empty() || v.index < 0 || v.index > 2 || !CanEditSlides()) return;
      ed::AnimTimingPatch p;
      p.trigger = static_cast<ed::AnimTrigger>(v.index);
      Done(ed::SetAnimationTiming(*ed2, current_, t, p));
    };
  }
  spin(CmdAnimDuration, L"Duration", L"DU", duration, 59, [this](double s) {
    edit::Editor* ed2 = SlideEditor();
    const std::vector<size_t> t = AnimTargets();
    if (!ed2 || t.empty() || !CanEditSlides()) return;
    ed::AnimTimingPatch p;
    p.durationMs = std::clamp(s, 0.01, 59.0) * 1000;
    Done(ed::SetAnimationTiming(*ed2, current_, t, p));
  });
  spin(CmdAnimDelay, L"Delay", L"DE", delay, 59, [this](double s) {
    edit::Editor* ed2 = SlideEditor();
    const std::vector<size_t> t = AnimTargets();
    if (!ed2 || t.empty() || !CanEditSlides()) return;
    ed::AnimTimingPatch p;
    p.delayMs = std::clamp(s, 0.0, 59.0) * 1000;
    Done(ed::SetAnimationTiming(*ed2, current_, t, p));
  });
  add(CmdAnimEarlier, icon::ChevronUp, RibbonSize::Medium, L"Move Earlier", L"ME").newColumn = true;
  add(CmdAnimLater, icon::ChevronDown, RibbonSize::Medium, L"Move Later", L"ML");
}

// ---------------------------------------------------------------------------------------------
// Commands

std::vector<int> PresentationView::AnimCommands() const {
  std::vector<int> out = {CmdTransitionPreview, CmdTransitionApplyAll, CmdTransitionOnClick, CmdTransitionAfter,
                          CmdAnimPreview,       CmdAnimPane,           CmdAnimEarlier,       CmdAnimLater, CmdAnimRemove};
  return out;
}

bool PresentationView::AnimCommandState(int cmd, CommandState& s) {
  if (cmd < CmdTransitionPreview || cmd > CmdAnimByParagraph) return false;
  edit::Editor* e = SlideEditor();
  const bool ready = Ready() && SlideCount() > 0 && e;
  const ed::TransitionInfo tr = ready ? AnimNow().transition : ed::TransitionInfo{};
  const bool timed = tr.effect != ed::TransitionEffect::None && tr.effect != ed::TransitionEffect::Cut;
  if (cmd >= CmdTransitionOption && cmd < CmdTransitionOption + 8) {
    const std::vector<std::u16string> opts = ed::TransitionOptions(tr.effect);
    const size_t i = static_cast<size_t>(cmd - CmdTransitionOption);
    s.enabled = ready && i < opts.size();
    s.checked = s.enabled && tr.option == static_cast<int>(i);
    s.label = i < opts.size() ? W(opts[i]) : L"Effect option " + std::to_wstring(i + 1);
    return true;
  }
  if (cmd >= CmdAnimOption && cmd < CmdAnimOption + 8) {
    const std::vector<size_t> t = ready ? AnimTargets() : std::vector<size_t>{};
    const std::vector<ed::AnimEffect> seq = ready ? AnimNow().seq : std::vector<ed::AnimEffect>{};
    ed::Anim a = t.empty() ? ed::Anim::Other : seq[t.front()].anim;
    int option = t.empty() ? -1 : seq[t.front()].option;
    bool editable = !t.empty();
    for (size_t k : t) {
      editable = editable && seq[k].editable;
      if (seq[k].anim != a) a = ed::Anim::Other;
      if (seq[k].option != option) option = -1;
    }
    const std::vector<std::u16string> opts = ed::AnimOptions(a);
    const size_t i = static_cast<size_t>(cmd - CmdAnimOption);
    s.enabled = editable && i < opts.size();
    s.checked = s.enabled && option == static_cast<int>(i);
    s.label = i < opts.size() ? W(opts[i]) : L"Effect option " + std::to_wstring(i + 1);
    return true;
  }
  switch (cmd) {
    case CmdTransitionPreview:
      s.enabled = ready && tr.effect != ed::TransitionEffect::None;
      s.label = L"Preview the transition";
      break;
    case CmdTransitionStyles:
      s.enabled = ready;
      s.label = L"Transition to this slide";
      break;
    case CmdTransitionOptionsMenu:
      s.enabled = ready && !ed::TransitionOptions(tr.effect).empty();
      s.label = L"Transition effect options";
      break;
    case CmdTransitionDuration:
      s.enabled = ready && timed;
      s.label = L"Transition duration";
      break;
    case CmdTransitionApplyAll:
      s.enabled = ready && SlideCount() > 1;
      s.label = L"Apply this transition to all slides";
      break;
    case CmdTransitionOnClick:
      s.enabled = ready;
      s.checked = tr.advanceOnClick;
      s.label = L"Advance slide on mouse click";
      break;
    case CmdTransitionAfter:
      s.enabled = ready;
      s.checked = tr.advanceAfterMs >= 0;
      s.label = L"Advance slide after a time";
      break;
    case CmdTransitionAfterTime:
      s.enabled = ready;
      s.label = L"Advance after (seconds)";
      break;
    case CmdAnimPreview:
      s.enabled = ready && !AnimNow().seq.empty();
      s.label = L"Preview the animations";
      break;
    case CmdAnimStyles:
      s.enabled = ready && (!Objects().Selection().empty() || !AnimTargets().empty());
      s.label = L"Animation";
      break;
    case CmdAnimAddMenu:
      s.enabled = ready && !Objects().Selection().empty();
      s.label = L"Add animation";
      break;
    case CmdAnimOptionsMenu:
    case CmdAnimAsOneObject:
    case CmdAnimByParagraph: {
      const std::vector<size_t> t = ready ? AnimTargets() : std::vector<size_t>{};
      const std::vector<ed::AnimEffect> seq = ready ? AnimNow().seq : std::vector<ed::AnimEffect>{};
      const Slide* slide = ready && deck_->Model() ? const_cast<Presentation*>(deck_->Model())->GetSlide(current_) : nullptr;
      const bool sequence = SequenceApplies(slide, seq, t);
      if (cmd == CmdAnimOptionsMenu) {
        s.enabled = !t.empty() && seq[t.front()].editable && (!ed::AnimOptions(seq[t.front()].anim).empty() || sequence);
        s.label = L"Animation effect options";
      } else {
        s.enabled = sequence;
        s.checked = sequence && (seq[t.front()].paragraph >= 0) == (cmd == CmdAnimByParagraph);
        s.label = cmd == CmdAnimByParagraph ? L"By Paragraph" : L"As One Object";
      }
      break;
    }
    case CmdAnimPane:
      s.enabled = Ready();
      s.checked = animPane_ && host_.ViewShownPanel() == animPane_.get();
      s.label = L"Animation pane";
      break;
    case CmdAnimStart:
    case CmdAnimDelay:
      s.enabled = ready && !AnimTargets().empty();
      s.label = cmd == CmdAnimStart ? L"Animation start" : L"Animation delay";
      break;
    case CmdAnimDuration: {
      const std::vector<size_t> t = ready ? AnimTargets() : std::vector<size_t>{};
      const std::vector<ed::AnimEffect> seq = ready ? AnimNow().seq : std::vector<ed::AnimEffect>{};
      s.enabled = !t.empty() && std::all_of(t.begin(), t.end(), [&](size_t k) { return seq[k].editable && seq[k].durationMs > 0; });
      s.label = L"Animation duration";
      break;
    }
    case CmdAnimEarlier:
    case CmdAnimLater: {
      const std::vector<size_t> t = ready ? AnimTargets() : std::vector<size_t>{};
      const size_t n = ready ? AnimNow().seq.size() : 0;
      s.enabled = !t.empty() && (cmd == CmdAnimEarlier ? t.front() > 0 : t.back() + 1 < n);
      s.label = cmd == CmdAnimEarlier ? L"Move animation earlier" : L"Move animation later";
      break;
    }
    case CmdAnimRemove:
      s.enabled = ready && !AnimTargets().empty();
      s.label = L"Remove animation";
      break;
    default:
      return false;
  }
  return true;
}

bool PresentationView::AnimCommand(int cmd) {
  if (cmd < CmdTransitionPreview || cmd > CmdAnimByParagraph) return false;
  edit::Editor* e = SlideEditor();
  if (!Ready() || !SlideCount() || !e) return true;
  if (cmd == CmdAnimPane) {
    if (animPane_ && host_.ViewShownPanel() == animPane_.get()) {
      host_.ViewShowPanel(nullptr);
    } else {
      if (!animPane_) animPane_ = std::make_unique<AnimationPane>(*this);
      host_.ViewShowPanel(animPane_.get());
    }
    return true;
  }
  if (cmd == CmdTransitionPreview) {
    StartPreview(true);
    return true;
  }
  if (cmd == CmdAnimPreview) {
    StartPreview(false);
    return true;
  }
  if (cmd == CmdTransitionStyles || cmd == CmdAnimStyles || cmd == CmdAnimAddMenu || cmd == CmdTransitionOptionsMenu ||
      cmd == CmdAnimOptionsMenu || cmd == CmdTransitionDuration || cmd == CmdTransitionAfterTime || cmd == CmdAnimStart ||
      cmd == CmdAnimDuration || cmd == CmdAnimDelay)
    return true;  // their galleries, menus and boxes do the work
  if (!CanEditSlides()) return true;
  const ed::TransitionInfo tr = AnimNow().transition;
  if (cmd >= CmdTransitionOption && cmd < CmdTransitionOption + 8) {
    ed::TransitionPatch p;
    p.option = cmd - CmdTransitionOption;
    Done(ed::SetTransition(*e, SelectedSlides(), p));
    return true;
  }
  if (cmd >= CmdAnimOption && cmd < CmdAnimOption + 8) {
    const std::vector<size_t> t = AnimTargets();
    if (!t.empty()) Done(ed::SetAnimationOption(*e, current_, t, cmd - CmdAnimOption));
    return true;
  }
  switch (cmd) {
    case CmdTransitionApplyAll: Done(ed::ApplyTransitionToAll(*e, current_)); break;
    case CmdTransitionOnClick: {
      ed::TransitionPatch p;
      p.advanceOnClick = !tr.advanceOnClick;
      Done(ed::SetTransition(*e, SelectedSlides(), p));
      break;
    }
    case CmdTransitionAfter: {
      ed::TransitionPatch p;
      p.advanceAfterMs = tr.advanceAfterMs >= 0 ? -1.0 : 0.0;
      Done(ed::SetTransition(*e, SelectedSlides(), p));
      break;
    }
    case CmdAnimEarlier:
    case CmdAnimLater: {
      const std::vector<size_t> t = AnimTargets();
      const size_t n = AnimNow().seq.size();
      if (t.empty() || (cmd == CmdAnimEarlier ? t.front() == 0 : t.back() + 1 >= n)) break;
      const size_t to = cmd == CmdAnimEarlier ? t.front() - 1 : t.front() + 1;
      if (Done(ed::MoveAnimations(*e, current_, t, to))) {
        std::vector<size_t> moved;
        for (size_t k = 0; k < t.size(); ++k) moved.push_back(to + k);
        PickAnims(moved);
      }
      break;
    }
    case CmdAnimRemove: {
      const std::vector<size_t> t = AnimTargets();
      if (!t.empty() && Done(ed::RemoveAnimations(*e, current_, t))) animPicked_.clear();
      break;
    }
    case CmdAnimAsOneObject:
    case CmdAnimByParagraph: {
      // One picked effect per shape, the last first: each change leaves the effects before it in place.
      const std::vector<ed::AnimEffect> seq = AnimNow().seq;
      std::vector<size_t> one;
      for (size_t k : AnimTargets())
        if (k < seq.size() && std::none_of(one.begin(), one.end(), [&](size_t j) { return seq[j].shape == seq[k].shape; }))
          one.push_back(k);
      std::sort(one.rbegin(), one.rend());
      edit::EditGroup group(*e, u"Effect Options");
      bool ok = !one.empty();
      for (size_t k : one) ok = ok && Done(ed::SetAnimationSequence(*e, current_, k, cmd == CmdAnimByParagraph));
      group.done = ok;
      animPicked_.clear();
      break;
    }
    default: break;
  }
  AnimChanged();
  return true;
}

// Script steps (snapshots and tests): transition:N and anim:N pick gallery choice N (0: None),
// animadd:N adds animation N to the selected shapes, animpane opens the pane, animpick:1,3 picks
// effects there (1-based), animdump prints the slide's transition and sequence to stderr.
bool PresentationView::AnimScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (op != L"transition" && op != L"anim" && op != L"animadd" && op != L"animpane" && op != L"animpick" && op != L"animdump" &&
      op != L"preview")
    return false;
  edit::Editor* e = SlideEditor();
  if (!Ready() || !SlideCount() || !e) return true;
  int n = -1;
  ParseInt(arg, n);
  if (op == L"transition" && n >= 0 && n < static_cast<int>(std::size(kTransitionGallery))) {
    ed::TransitionPatch p;
    p.effect = kTransitionGallery[n];
    Done(ed::SetTransition(*e, SelectedSlides(), p));
  } else if (op == L"anim" || op == L"animadd") {
    if (n >= 0) ApplyAnim(n, op == L"animadd");
  } else if (op == L"preview") {
    // preview:transition@400 or preview:animations@300 (or animations:3@300, from effect 3): the
    // preview held at that many ms, for a snapshot.
    const size_t at = arg.find(L'@');
    const std::wstring what = arg.substr(0, at);
    const size_t colon = what.find(L':');
    int from = 1, ms = 0;
    if (colon != std::wstring::npos) ParseInt(what.substr(colon + 1), from);
    if (at != std::wstring::npos) ParseInt(arg.substr(at + 1), ms);
    StartPreview(what.rfind(L"transition", 0) == 0, static_cast<size_t>(std::max(1, from) - 1));
    if (preview_) preview_->fixedAt = std::max(0, ms);
  } else if (op == L"animpane") {
    if (!animPane_) animPane_ = std::make_unique<AnimationPane>(*this);
    host_.ViewShowPanel(animPane_.get());
  } else if (op == L"animpick") {
    std::vector<size_t> pick;
    for (const std::wstring& part : fw::Split(arg, L',')) {
      int k = 0;
      if (ParseInt(part, k) && k >= 1) pick.push_back(static_cast<size_t>(k - 1));
    }
    PickAnims(pick);
  } else if (op == L"animdump") {
    const ed::TransitionInfo t = AnimNow().transition;
    fwprintf(stderr, L"animdump: transition %ls option %d, %.0f ms, click %d, after %.0f ms\n", W(ed::TransitionName(t.effect)).c_str(),
             t.option, t.durationMs, t.advanceOnClick ? 1 : 0, t.advanceAfterMs);
    const std::vector<ed::AnimEffect> seq = AnimNow().seq;
    const std::vector<size_t> targets = AnimTargets();
    for (size_t i = 0; i < seq.size(); ++i) {
      const ed::AnimEffect& a = seq[i];
      const std::wstring paragraph = a.paragraph >= 0 ? L" paragraph " + std::to_wstring(a.paragraph) : L"";
      fwprintf(stderr, L"animdump: %zu. shape %u%ls %ls option %d, %ls, %.0f ms, delay %.0f ms%ls\n", i + 1, a.shape,
               paragraph.c_str(), W(ed::AnimName(a.anim)).c_str(), a.option,
               a.trigger == ed::AnimTrigger::OnClick ? L"click" : a.trigger == ed::AnimTrigger::WithPrevious ? L"with" : L"after",
               a.durationMs, a.delayMs, std::find(targets.begin(), targets.end(), i) != targets.end() ? L", selected" : L"");
    }
    fflush(stderr);
  }
  return true;
}

}  // namespace fw::slide::view
