// The presentation tab's design (P2, p2-slides; plan §8.2): the Design panel (built-in themes and
// theme colors, the background, the slide size) and the Header & footer panel, their commands, and
// the design edits, which go through the edit layer.


#include <algorithm>
#include <cmath>
#include <map>

#include "Commands.h"
#include "Util.h"
#include "core/Strings.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/SlideDesignOps.h"
#include "slide/view/ObjectTool.h"
#include "slide/view/PresentationPanels.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;
using namespace gfx;

namespace fw::slide::view {

namespace {

constexpr float kThemeW = 136, kThemeH = 77, kThemeName = 20, kGap = 10, kColorRow = 26;

D2D1_COLOR_F Col(uint32_t rgb) { return Rgb(rgb & 0xFFFFFF); }

// The deck's theme colors as the color pickers list them: background 1, text 1, background 2,
// text 2, accents 1-6.
std::vector<NamedColor> PickerTheme(const SlideBase* slide) {
  std::vector<NamedColor> out = ColorPalette::DefaultTheme();
  if (!slide || !slide->theme) return out;
  const draw::ThemeColor order[10] = {draw::ThemeColor::Light1,  draw::ThemeColor::Dark1,   draw::ThemeColor::Light2,
                                      draw::ThemeColor::Dark2,   draw::ThemeColor::Accent1, draw::ThemeColor::Accent2,
                                      draw::ThemeColor::Accent3, draw::ThemeColor::Accent4, draw::ThemeColor::Accent5,
                                      draw::ThemeColor::Accent6};
  for (int i = 0; i < 10; ++i) {
    const draw::Color c = slide->theme->GetColor(order[i]);
    out[static_cast<size_t>(i)].color = D2D1::ColorF(c.r, c.g, c.b, 1);
  }
  return out;
}

uint32_t ToRgb(D2D1_COLOR_F c) {
  auto ch = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
  return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

}  // namespace

// ---------------------------------------------------------------------------
// Design

PresentationView::DesignPanel::DesignPanel(PresentationView& view) : v_(view) {
  const wchar_t* names[kTabs] = {L"Themes", L"Background", L"Slide size"};
  for (int i = 0; i < kTabs; ++i) {
    tabs_[i].text = names[i];
    tabs_[i].toggle = true;
    tabs_[i].textSize = 12;
    tabs_[i].onInvoke = [this, i] { Open(static_cast<Tab>(i)); };
  }
  const wchar_t* kinds[3] = {L"Solid", L"Gradient", L"Picture"};
  for (int i = 0; i < 3; ++i) {
    kind_[i].text = kinds[i];
    kind_[i].toggle = true;
    kind_[i].textSize = 12;
    kind_[i].onInvoke = [this, i] {
      bg_.kind = i == 0 ? Background::Kind::Solid : i == 1 ? Background::Kind::Gradient : Background::Kind::Picture;
      if (bg_.kind == Background::Kind::Picture && picturePath_.empty()) {
        picture_.onInvoke();  // a picture fill starts with choosing the picture
        return;
      }
      ApplyBackground();
    };
  }
  color1_.label = L"Color";
  color1_.onChange = [this](D2D1_COLOR_F c, bool) {
    bg_.color = ToRgb(c);
    if (bg_.kind != Background::Kind::Gradient) bg_.kind = Background::Kind::Solid;
    ApplyBackground();
  };
  color2_.label = L"Second color";
  color2_.onChange = [this](D2D1_COLOR_F c, bool) {
    bg_.color2 = ToRgb(c);
    bg_.kind = Background::Kind::Gradient;
    ApplyBackground();
  };
  for (const GradientPreset& g : GradientPresets()) direction_.items.push_back(g.name);
  direction_.label = L"Direction";
  direction_.onChange = [this](int i) {
    if (i < 0 || static_cast<size_t>(i) >= GradientPresets().size()) return;
    bg_.angle = GradientPresets()[static_cast<size_t>(i)].angle;
    bg_.kind = Background::Kind::Gradient;
    ApplyBackground();
  };
  picture_.text = L"Choose picture…";
  picture_.textSize = 12;
  picture_.onInvoke = [this] {
    // The pictures dialog Insert > Pictures uses (p2-objects); SetBackground reads and stores the file.
    const std::vector<std::wstring> files = PickPictureFiles(v_.host_.ViewHwnd(), false);
    if (files.empty()) return;
    picturePath_ = files.front();
    bg_.kind = Background::Kind::Picture;
    ApplyBackground();
  };
  hide_.text = L"Hide background graphics";
  hide_.toggle = true;
  hide_.textSize = 12;
  hide_.onInvoke = [this] {
    bg_.hideGraphics = hide_.checked;
    ApplyBackground();
  };
  all_.text = L"Apply to all slides";
  all_.textSize = 12;
  all_.onInvoke = [this] {
    std::vector<size_t> every(v_.SlideCount());
    for (size_t i = 0; i < every.size(); ++i) every[i] = i;
    v_.SetBackground(every, bg_, picturePath_);
  };
  reset_.text = L"Reset background";
  reset_.textSize = 12;
  reset_.onInvoke = [this] { v_.ResetBackground(v_.SelectedSlides()); };
  for (int i = 0; i < 2; ++i) {
    sizes_[i].text = SlideSizes()[static_cast<size_t>(i)].name;
    sizes_[i].toggle = true;
    sizes_[i].textSize = 12;
    sizes_[i].onInvoke = [this, i] {
      v_.SetSlideSize(i, scale_.selected == 1 ? static_cast<int>(ContentScale::Maximize) : static_cast<int>(ContentScale::EnsureFit));
      Sync();
    };
  }
  scale_.items = {L"Ensure fit", L"Maximize"};
  scale_.selected = 0;
  scale_.label = L"When the slide gets smaller";
}

void PresentationView::DesignPanel::Open(Tab tab) {
  tab_ = tab;
  scroll_ = 0;
  Sync();
}

void PresentationView::DesignPanel::Sync() {
  Presentation* model = v_.deck_ ? v_.deck_->Model() : nullptr;
  const Slide* slide = model && v_.SlideCount() ? model->GetSlide(v_.current_) : nullptr;
  for (int i = 0; i < kTabs; ++i) tabs_[i].checked = i == tab_;
  currentColors_ = slide ? ColorSchemeOf(*slide) : -1;
  if (slide) bg_ = ReadBackground(*slide);
  kind_[0].checked = bg_.kind == Background::Kind::Solid;
  kind_[1].checked = bg_.kind == Background::Kind::Gradient;
  kind_[2].checked = bg_.kind == Background::Kind::Picture;
  const std::vector<NamedColor> theme = PickerTheme(slide);
  color1_.theme = color2_.theme = theme;
  color1_.color = Col(bg_.kind == Background::Kind::Theme ? 0xFFFFFF : bg_.color);
  color2_.color = Col(bg_.color2);
  direction_.selected = -1;
  for (size_t i = 0; i < GradientPresets().size(); ++i)
    if (GradientPresets()[i].angle == bg_.angle) direction_.selected = static_cast<int>(i);
  hide_.checked = bg_.hideGraphics;
  sizeChoice_ = v_.deck_ ? SlideSizeChoiceOf(v_.deck_->SlideSize()) : -1;
  for (int i = 0; i < 2; ++i) sizes_[i].checked = i == sizeChoice_;
  v_.host_.ViewInvalidate();
}

void PresentationView::DesignPanel::ApplyBackground() {
  v_.SetBackground(v_.SelectedSlides(), bg_, bg_.kind == Background::Kind::Picture ? picturePath_ : std::wstring());
  Sync();
}

std::vector<Control*> PresentationView::DesignPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  body_ = body;
  labels_.clear();
  std::vector<Control*> out;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui, mid = (left + right) / 2, gap = 4 * ui;
  const float rowH = 30 * ui;
  float y = body.top + 10 * ui;
  for (Control* c : std::initializer_list<Control*>{&kind_[0], &kind_[1], &kind_[2], &picture_, &hide_, &all_, &reset_, &color1_,
                                                    &color2_, &direction_, &sizes_[0], &sizes_[1], &scale_})
    c->visible = false;
  const float tabW = (right - left) / static_cast<float>(kTabs);
  for (int i = 0; i < kTabs; ++i) {
    tabs_[i].rc = RectF(left + i * tabW, y, left + (i + 1) * tabW - 2 * ui, y + 28 * ui);
    out.push_back(&tabs_[i]);
  }
  y += 28 * ui + 14 * ui;
  auto label = [&](const std::wstring& text, float l, float r) { labels_.push_back({text, RectF(l, y, r, y + 16 * ui)}); };
  auto place = [&](Control& c, float l, float r) {
    c.rc = RectF(l, y, r, y + rowH);
    c.visible = true;
    out.push_back(&c);
  };
  switch (tab_) {
    case kThemes: area_ = RectF(left, y, right, body.bottom); break;
    case kBackground: {
      label(L"Fill", left, right);
      y += 18 * ui;
      const float w = (right - left - 2 * gap) / 3;
      for (int i = 0; i < 3; ++i) place(kind_[i], left + i * (w + gap), left + i * (w + gap) + w);
      y += rowH + 12 * ui;
      if (bg_.kind == Background::Kind::Picture) {
        place(picture_, left, right);
        y += rowH + 10 * ui;
      } else {
        const bool grad = bg_.kind == Background::Kind::Gradient;
        label(grad ? L"From" : L"Color", left, mid - gap);
        if (grad) label(L"To", mid + gap, right);
        y += 18 * ui;
        place(color1_, left, mid - gap);
        if (grad) place(color2_, mid + gap, right);
        y += rowH + 10 * ui;
        if (grad) {
          label(L"Direction", left, right);
          y += 18 * ui;
          place(direction_, left, right);
          y += rowH + 10 * ui;
        }
      }
      place(hide_, left, right);
      y += rowH + 18 * ui;
      place(all_, left, mid - gap);
      place(reset_, mid + gap, right);
      y += rowH + 10 * ui;
      labels_.push_back({L"Changes apply to the selected slides as you make them.", RectF(left, y, right, y + 34 * ui)});
      break;
    }
    case kSize:
      label(L"Slide size", left, right);
      y += 18 * ui;
      place(sizes_[0], left, mid - gap);
      place(sizes_[1], mid + gap, right);
      y += rowH + 14 * ui;
      label(L"When the slide gets smaller", left, right);
      y += 18 * ui;
      place(scale_, left, right);
      y += rowH + 10 * ui;
      labels_.push_back({L"Ensure fit scales the content down so all of it stays on the slide; Maximize keeps it as large as "
                         L"it is and lets it run over the sides.",
                         RectF(left, y, right, y + 60 * ui)});
      break;
    default: break;
  }
  return out;
}

D2D1_RECT_F PresentationView::DesignPanel::ThemeRect(int i) const {
  const float u = ui_, w = kThemeW * u, h = (kThemeH + kThemeName) * u;
  const int col = i % 2, row = i / 2;
  const float x = area_.left + col * (w + kGap * u), y = area_.top + 20 * u + row * (h + kGap * u) - scroll_;
  return RectF(std::round(x), std::round(y), std::round(x + w), std::round(y + kThemeH * u));
}

D2D1_RECT_F PresentationView::DesignPanel::ColorsRect(int i) const {
  const float u = ui_;
  const int rows = (static_cast<int>(BuiltInThemes().size()) + 1) / 2;
  const float top = area_.top + 20 * u + rows * (kThemeH + kThemeName + kGap) * u + 30 * u - scroll_;
  return RectF(area_.left, top + i * kColorRow * u, area_.right, top + (i + 1) * kColorRow * u);
}

float PresentationView::DesignPanel::ContentHeight() const {
  const int n = static_cast<int>(ColorSchemes().size());
  return ColorsRect(n - 1).bottom + scroll_ - area_.top + 8 * ui_;
}

PresentationView::DesignPanel::Hit PresentationView::DesignPanel::At(float x, float y) const {
  Hit h;
  if (tab_ != kThemes || !Contains(area_, x, y)) return h;
  for (int i = 0; i < static_cast<int>(BuiltInThemes().size()); ++i) {
    D2D1_RECT_F r = ThemeRect(i);
    r.bottom += kThemeName * ui_;
    if (Contains(r, x, y)) h.theme = i;
  }
  for (int i = 0; i < static_cast<int>(ColorSchemes().size()); ++i)
    if (Contains(ColorsRect(i), x, y)) h.colors = i;
  return h;
}

namespace {

// A theme's heading font for its preview, kept for the process (a few themes, a few sizes).
IDWriteTextFormat* PreviewFont(const char* family, float size) {
  static std::map<std::pair<std::string, int>, ComPtr<IDWriteTextFormat>> fonts;
  const auto key = std::make_pair(std::string(family), static_cast<int>(size * 4));
  auto it = fonts.find(key);
  if (it != fonts.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> f;
  const std::wstring name(family, family + std::char_traits<char>::length(family));
  if (FAILED(DWrite()->CreateTextFormat(name.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
    return nullptr;
  f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  return (fonts[key] = f).Get();
}

}  // namespace

void DrawThemePreview(Canvas& c, int i, const D2D1_RECT_F& r) {
  const BuiltInTheme& t = BuiltInThemes()[static_cast<size_t>(i)];
  const ColorScheme& cs = ColorSchemes()[static_cast<size_t>(t.colors)];
  const uint32_t base = t.dark ? cs.colors[0] : cs.colors[1];
  switch (t.background.kind) {
    case Background::Kind::Gradient: {
      // A few bands of the gradient, top to bottom.
      const int bands = 12;
      for (int k = 0; k < bands; ++k) {
        const float f = (k + 0.5f) / bands;
        const D2D1_COLOR_F col = Mix(Col(t.background.color), Col(t.background.color2), f);
        c.Fill(RectF(r.left, r.top + Height(r) * k / bands, r.right, r.top + Height(r) * (k + 1) / bands + 0.5f), col);
      }
      break;
    }
    case Background::Kind::Solid: c.Fill(r, Col(t.background.color)); break;
    default: c.Fill(r, Col(base)); break;
  }
  // "Aa" in the heading font and the six accents, as big as the picture allows.
  const float k = Height(r) / kThemeH;  // the panel's picture is kThemeH DIPs high
  const uint32_t text = t.dark ? cs.colors[1] : cs.colors[0];
  if (IDWriteTextFormat* f = PreviewFont(t.major, 24 * k))
    c.DC()->DrawText(L"Aa", 2, f, RectF(r.left + 10 * k, r.top + 8 * k, r.right, r.bottom), c.Brush(Col(text)));
  const float w = 9 * k, h = 9 * k;
  for (int j = 0; j < 6; ++j) {
    const float x = r.left + 10 * k + j * (w + 3 * k), y = r.bottom - h - 9 * k;
    c.Fill(RectF(x, y, x + w, y + h), Col(cs.colors[4 + j]));
  }
}

void DrawColorsPreview(Canvas& c, int i, const D2D1_RECT_F& r, float ui) {
  const ColorScheme& cs = ColorSchemes()[static_cast<size_t>(i)];
  const int shown[8] = {2, 3, 4, 5, 6, 7, 8, 9};  // dark 2, light 2, accents
  const float sw = 14 * ui;
  for (int k = 0; k < 8; ++k) {
    const float x = r.left + 6 * ui + k * (sw + 1 * ui), y = (r.top + r.bottom - sw) / 2;
    c.Fill(RectF(x, y, x + sw, y + sw), Col(cs.colors[shown[k]]));
  }
  c.Text(cs.name, RectF(r.left + 6 * ui + 8 * (sw + ui) + 8 * ui, r.top, r.right, r.bottom), theme::P().text, 12 * ui);
}

void PresentationView::DesignPanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  for (const auto& [text, rc] : labels_) c.TextWrapped(text, rc, p.textSecondary, 12 * ui_, 3);
  if (tab_ != kThemes) return;
  c.PushClip(area_);
  c.Text(L"Themes", RectF(area_.left, area_.top - scroll_, area_.right, area_.top + 18 * ui_ - scroll_), p.textSecondary, 12 * ui_);
  for (int i = 0; i < static_cast<int>(BuiltInThemes().size()); ++i) {
    const D2D1_RECT_F r = ThemeRect(i);
    if (r.bottom + kThemeName * ui_ < area_.top || r.top > area_.bottom) continue;
    if (hover_.theme == i) c.FillRound(Inflate(RectF(r.left, r.top, r.right, r.bottom + kThemeName * ui_), 3 * ui_, 3 * ui_), 4 * ui_, p.hover);
    DrawThemePreview(c, i, r);
    c.Stroke(r, Alpha(p.chromeBorder, 0.9f), std::max(1.f, ui_ * 0.75f));
    c.Text(BuiltInThemes()[static_cast<size_t>(i)].name, RectF(r.left, r.bottom + 2 * ui_, r.right, r.bottom + kThemeName * ui_), p.text, 12 * ui_);
  }
  const D2D1_RECT_F first = ColorsRect(0);
  c.Text(L"Theme colors", RectF(area_.left, first.top - 24 * ui_, area_.right, first.top - 6 * ui_), p.textSecondary, 12 * ui_);
  for (int i = 0; i < static_cast<int>(ColorSchemes().size()); ++i) {
    const D2D1_RECT_F r = ColorsRect(i);
    if (r.bottom < area_.top || r.top > area_.bottom) continue;
    if (hover_.colors == i) c.FillRound(r, 4 * ui_, p.hover);
    if (currentColors_ == i) c.StrokeRound(Inflate(r, -1, -1), 4 * ui_, p.accentText, 1.5f * ui_);
    DrawColorsPreview(c, i, r, ui_);
  }
  c.PopClip();
}

bool PresentationView::DesignPanel::OnMouseDown(float x, float y, bool) {
  const Hit h = At(x, y);
  if (h.theme >= 0) {
    v_.ApplyTheme(h.theme, {});
    Sync();
    return true;
  }
  if (h.colors >= 0) {
    v_.ApplyColors(h.colors, {});
    Sync();
    return true;
  }
  return false;
}

bool PresentationView::DesignPanel::OnMouseMove(float x, float y) {
  const Hit h = At(x, y);
  const bool changed = h.theme != hover_.theme || h.colors != hover_.colors;
  hover_ = h;
  return changed;
}

void PresentationView::DesignPanel::OnMouseLeave() { hover_ = Hit{}; }

bool PresentationView::DesignPanel::OnWheel(int delta, float x, float y) {
  if (tab_ != kThemes || !Contains(area_, x, y)) return false;
  const float max = std::max(0.f, ContentHeight() - Height(area_));
  scroll_ = std::clamp(scroll_ - static_cast<float>(delta) / WHEEL_DELTA * 48 * ui_, 0.f, max);
  return true;
}

std::wstring PresentationView::DesignPanel::TooltipAt(float x, float y) {
  const Hit h = At(x, y);
  if (h.theme >= 0) return std::wstring(L"Apply the ") + BuiltInThemes()[static_cast<size_t>(h.theme)].name + L" theme to the presentation";
  if (h.colors >= 0) return std::wstring(L"Use the ") + ColorSchemes()[static_cast<size_t>(h.colors)].name + L" theme colors";
  return {};
}

// ---------------------------------------------------------------------------
// Header & footer

PresentationView::HeaderFooterPanel::HeaderFooterPanel(PresentationView& view) : v_(view) {
  auto toggle = [&](PanelButton& b, const wchar_t* text, std::function<void()> fn) {
    b.text = text;
    b.toggle = true;
    b.textSize = 12;
    b.onInvoke = std::move(fn);
  };
  toggle(date_, L"Date and time", [this] { s_.date = date_.checked; Refresh(); });
  toggle(auto_, L"Update automatically", [this] { s_.dateAuto = true; Refresh(); });
  toggle(fixed_, L"Fixed", [this] { s_.dateAuto = false; Refresh(); });
  toggle(number_, L"Slide number", [this] { s_.slideNumber = number_.checked; Refresh(); });
  toggle(footer_, L"Footer", [this] { s_.footer = footer_.checked; Refresh(); });
  toggle(notTitle_, L"Don't show on title slide", [this] { s_.notOnTitle = notTitle_.checked; Refresh(); });
  format_.label = L"Date format";
  format_.onChange = [this](int i) { s_.dateFormat = i + 1; };
  fixedText_.autoComplete = false;
  fixedText_.label = L"Fixed date";
  fixedText_.onCommit = [this](const std::wstring& t, int) { s_.dateFixed = t; };
  footerText_.autoComplete = false;
  footerText_.label = L"Footer text";
  footerText_.onCommit = [this](const std::wstring& t, int) {
    s_.footerText = t;
    s_.footer = s_.footer || !t.empty();
    Refresh();
  };
  apply_.text = L"Apply";
  apply_.textSize = 12;
  apply_.onInvoke = [this] { v_.ApplyHeaderFooter(s_, v_.SelectedSlides()); };
  applyAll_.text = L"Apply to all";
  applyAll_.textSize = 12;
  applyAll_.onInvoke = [this] { v_.ApplyHeaderFooter(s_, {}); };
}

void PresentationView::HeaderFooterPanel::Sync() {
  Presentation* model = v_.deck_ ? v_.deck_->Model() : nullptr;
  const Slide* slide = model && v_.SlideCount() ? model->GetSlide(v_.current_) : nullptr;
  s_ = slide ? ReadHeaderFooter(*slide) : HeaderFooterSettings{};
  format_.items = DateFormatSamples(s_.lang.empty() ? std::string("en-US") : s_.lang);
  fixedText_.SetText(s_.dateFixed);
  footerText_.SetText(s_.footerText);
  Refresh();
}

void PresentationView::HeaderFooterPanel::Refresh() {
  date_.checked = s_.date;
  auto_.checked = s_.dateAuto;
  fixed_.checked = !s_.dateAuto;
  auto_.enabled = fixed_.enabled = format_.enabled = fixedText_.enabled = s_.date;
  format_.selected = std::clamp(s_.dateFormat, 1, 13) - 1;
  number_.checked = s_.slideNumber;
  footer_.checked = s_.footer;
  footerText_.enabled = s_.footer;
  notTitle_.checked = s_.notOnTitle;
  v_.host_.ViewInvalidate();
}

std::vector<Control*> PresentationView::HeaderFooterPanel::Layout(const D2D1_RECT_F& body, float ui) {
  ui_ = ui;
  labels_.clear();
  std::vector<Control*> out;
  const float left = body.left + 16 * ui, right = body.right - 16 * ui, mid = (left + right) / 2, gap = 4 * ui, rowH = 30 * ui;
  float y = body.top + 10 * ui;
  auto full = [&](Control& c) {
    c.rc = RectF(left, y, right, y + rowH);
    out.push_back(&c);
    y += rowH + 8 * ui;
  };
  auto pair = [&](Control& a, Control& b) {
    a.rc = RectF(left, y, mid - gap, y + rowH);
    b.rc = RectF(mid + gap, y, right, y + rowH);
    out.push_back(&a);
    out.push_back(&b);
    y += rowH + 8 * ui;
  };
  labels_.push_back({L"Shown on the slides", RectF(left, y, right, y + 16 * ui)});
  y += 20 * ui;
  full(date_);
  pair(auto_, fixed_);
  format_.visible = s_.dateAuto;
  fixedText_.visible = !s_.dateAuto;
  full(s_.dateAuto ? static_cast<Control&>(format_) : static_cast<Control&>(fixedText_));
  y += 6 * ui;
  full(number_);
  full(footer_);
  full(footerText_);
  y += 6 * ui;
  full(notTitle_);
  y += 10 * ui;
  pair(apply_, applyAll_);
  labels_.push_back({L"Apply sets the selected slides; Apply to all sets every slide.", RectF(left, y, right, y + 34 * ui)});
  return out;
}

void PresentationView::HeaderFooterPanel::Draw(Canvas& c) {
  const Palette& p = theme::P();
  for (const auto& [text, rc] : labels_) c.TextWrapped(text, rc, p.textSecondary, 12 * ui_, 2);
}

// ---------------------------------------------------------------------------
// Commands

void PresentationView::OpenDesign(int tab) {
  if (!designPanel_) designPanel_ = std::make_unique<DesignPanel>(*this);
  designPanel_->Open(static_cast<DesignPanel::Tab>(std::clamp(tab, 0, static_cast<int>(DesignPanel::kTabs) - 1)));
  host_.ViewShowPanel(designPanel_.get());
}

void PresentationView::OpenHeaderFooter() {
  if (!hfPanel_) hfPanel_ = std::make_unique<HeaderFooterPanel>(*this);
  hfPanel_->Sync();
  host_.ViewShowPanel(hfPanel_.get());
}

void PresentationView::DesignChanged() {
  PanelContent* shown = host_.ViewShownPanel();
  if (designPanel_ && shown == designPanel_.get()) designPanel_->Sync();
  if (hfPanel_ && shown == hfPanel_.get()) hfPanel_->Sync();
}

bool PresentationView::DesignCommandState(int cmd, CommandState& s) {
  const bool any = Ready() && SlideCount() > 0;
  PanelContent* shown = host_.ViewShownPanel();
  switch (cmd) {
    case CmdDesignPanel:
      s.enabled = Ready();
      s.checked = designPanel_ && shown == designPanel_.get();
      s.label = L"Design: themes, colors, background, slide size";
      return true;
    case CmdFormatBackground: s.enabled = any; s.label = L"Format background"; return true;
    case CmdResetBackground: s.enabled = any; s.label = L"Reset background"; return true;
    case CmdHeaderFooter:
      s.enabled = any;
      s.checked = hfPanel_ && shown == hfPanel_.get();
      s.label = L"Header & footer: slide numbers, footer and date";
      return true;
    case CmdSlideSizePanel: s.enabled = Ready(); s.label = L"Slide size"; return true;
    case CmdSlideSize169:
    case CmdSlideSize43: {
      const int choice = cmd == CmdSlideSize169 ? 0 : 1;
      s.enabled = Ready();
      s.checked = Ready() && SlideSizeChoiceOf(deck_->SlideSize()) == choice;
      s.label = std::wstring(L"Slide size: ") + SlideSizes()[static_cast<size_t>(choice)].name;
      return true;
    }
    default: break;
  }
  // The ribbon's Themes gallery and Colors list (r1-slide): the current slide's theme checked.
  const Slide* slide = any ? deck_->Model()->GetSlide(current_) : nullptr;
  if (cmd >= CmdApplyTheme && cmd < CmdApplyTheme + 10) {
    const size_t i = static_cast<size_t>(cmd - CmdApplyTheme);
    if (i >= BuiltInThemes().size()) return false;
    s.enabled = any;
    s.label = std::wstring(L"Theme: ") + BuiltInThemes()[i].name;
    s.checked = slide && slide->theme && Utf8ToWide(slide->theme->Name()) == BuiltInThemes()[i].name;
    return true;
  }
  if (cmd >= CmdApplyColors && cmd < CmdApplyColors + 25) {
    const size_t i = static_cast<size_t>(cmd - CmdApplyColors);
    if (i >= ColorSchemes().size()) return false;
    s.enabled = any;
    s.label = std::wstring(L"Theme colors: ") + ColorSchemes()[i].name;
    s.checked = slide && ColorSchemeOf(*slide) == static_cast<int>(i);
    return true;
  }
  return false;
}

bool PresentationView::DesignCommand(int cmd) {
  switch (cmd) {
    case CmdDesignPanel:
      if (designPanel_ && host_.ViewShownPanel() == designPanel_.get()) host_.ViewShowPanel(nullptr);
      else OpenDesign(DesignPanel::kThemes);
      return true;
    case CmdFormatBackground: OpenDesign(DesignPanel::kBackground); return true;
    case CmdSlideSizePanel: OpenDesign(DesignPanel::kSize); return true;
    case CmdResetBackground:
      if (SlideCount()) ResetBackground(SelectedSlides());
      return true;
    case CmdHeaderFooter:
      if (hfPanel_ && host_.ViewShownPanel() == hfPanel_.get()) host_.ViewShowPanel(nullptr);
      else OpenHeaderFooter();
      return true;
    case CmdSlideSize169:
    case CmdSlideSize43: SetSlideSize(cmd == CmdSlideSize169 ? 0 : 1, static_cast<int>(ContentScale::EnsureFit)); return true;
    default: break;
  }
  const int theme = cmd - CmdApplyTheme, colors = cmd - CmdApplyColors;
  if (theme >= 0 && theme < static_cast<int>(BuiltInThemes().size())) return ApplyTheme(theme, {}), true;
  if (colors >= 0 && colors < static_cast<int>(ColorSchemes().size())) return ApplyColors(colors, {}), true;
  return false;  // an open Design panel follows through DesignChanged
}

// ---------------------------------------------------------------------------
// Design edits (through the edit layer)

namespace {

constexpr std::string_view kRelImage = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image";
constexpr size_t kMaxPicture = size_t{64} << 20;  // bytes: a background picture bigger than this is refused

// The masters `slides` use (every master when empty), as indexes into the presentation's.
std::vector<size_t> MastersOf(Presentation& deck, const std::vector<size_t>& slides) {
  std::vector<size_t> out;
  const auto& masters = deck.Masters();
  if (slides.empty()) {
    for (size_t i = 0; i < masters.size(); ++i) out.push_back(i);
    return out;
  }
  for (size_t s : slides) {
    const Slide* slide = s < deck.SlideCount() ? deck.GetSlide(s) : nullptr;
    if (!slide || !slide->master) continue;
    for (size_t i = 0; i < masters.size(); ++i)
      if (masters[i].get() == slide->master && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
  }
  return out;
}

// A file name's extension, lower case, with its dot ("" when none).
std::wstring ExtensionOf(const std::wstring& path) {
  const size_t dot = path.find_last_of(L"./\\");
  std::wstring ext = dot != std::wstring::npos && path[dot] == L'.' ? path.substr(dot) : std::wstring();
  for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
  return ext;
}

// A picture's content type by its extension; empty for one a slide can't show.
std::string PictureType(const std::wstring& path) {
  const std::wstring ext = ExtensionOf(path);
  if (ext == L".png") return "image/png";
  if (ext == L".jpg" || ext == L".jpeg" || ext == L".jfif") return "image/jpeg";
  if (ext == L".gif") return "image/gif";
  if (ext == L".bmp") return "image/bmp";
  if (ext == L".tif" || ext == L".tiff") return "image/tiff";
  if (ext == L".emf") return "image/x-emf";
  if (ext == L".wmf") return "image/x-wmf";
  return {};
}

}  // namespace

bool PresentationView::ApplyTheme(int theme, const std::vector<size_t>& slides) {
  const auto& themes = BuiltInThemes();
  if (theme < 0 || static_cast<size_t>(theme) >= themes.size() || !CanEditSlides()) return false;
  const BuiltInTheme& t = themes[static_cast<size_t>(theme)];
  edit::ThemeDesign d;
  d.name = WideToUtf8(t.name);
  d.colorScheme = ColorSchemeXml(ColorSchemes()[static_cast<size_t>(t.colors)]);
  d.major = t.major;
  d.minor = t.minor;
  d.dark = t.dark ? 1 : 0;
  d.background = BackgroundXml(t.background);
  edit::Editor* ed = SlideEditor();
  const std::vector<size_t> masters = MastersOf(*deck_->Model(), slides);
  return Done(ed->Run(u"Theme", [&] {
    for (size_t m : masters)
      if (edit::Result r = edit::ApplyThemeDesign(*ed, m, d); !r) return r;
    return edit::Result{};
  }));
}

bool PresentationView::ApplyColors(int scheme, const std::vector<size_t>& slides) {
  const auto& schemes = ColorSchemes();
  if (scheme < 0 || static_cast<size_t>(scheme) >= schemes.size() || !CanEditSlides()) return false;
  edit::ThemeDesign d;
  d.step = u"Theme Colors";
  d.colorScheme = ColorSchemeXml(schemes[static_cast<size_t>(scheme)]);
  edit::Editor* ed = SlideEditor();
  const std::vector<size_t> masters = MastersOf(*deck_->Model(), slides);
  return Done(ed->Run(u"Theme Colors", [&] {
    for (size_t m : masters)
      if (edit::Result r = edit::ApplyThemeDesign(*ed, m, d); !r) return r;
    return edit::Result{};
  }));
}

bool PresentationView::SetBackground(const std::vector<size_t>& slides, const Background& bg, const std::wstring& picture) {
  if (slides.empty() || !CanEditSlides()) return false;
  edit::Editor* ed = SlideEditor();
  std::vector<uint8_t> bytes;
  std::string type;
  if (bg.kind == Background::Kind::Picture) {
    type = PictureType(picture);
    std::string read;
    if (type.empty() || !util::ReadFileBytes(picture, read, kMaxPicture) || read.empty()) {
      host_.ViewMessage(BannerKind::Warning, L"That picture can't be used as a background.");
      return false;
    }
    bytes.assign(read.begin(), read.end());
  }
  return Done(ed->Run(u"Format Background", [&] {
    std::string media;  // the picture's part, stored once for all the slides
    for (size_t s : slides) {
      std::string rel;
      if (!bytes.empty()) {
        const std::string part = ed->SlidePart(s);
        if (media.empty()) {
          media = ed->NewPartName("/ppt/media/image", WideToUtf8(ExtensionOf(picture)));
          rel = ed->AddPart(part, kRelImage, media, type, bytes);
        } else {
          rel = ed->AddRelationship(part, kRelImage, media);
        }
        if (rel.empty()) return edit::Result::Fail(u"The picture couldn't be added.");
      }
      const std::string xml = BackgroundXml(bg, rel);
      if (xml.empty()) return edit::Result::Fail(u"Choose a picture first.");
      if (edit::Result r = ed->SetBackground({s}, xml); !r) return r;
    }
    return edit::SetShowMasterShapes(*ed, slides, !bg.hideGraphics);
  }));
}

bool PresentationView::ResetBackground(const std::vector<size_t>& slides) {
  if (slides.empty() || !CanEditSlides()) return false;
  return Done(SlideEditor()->SetBackground(slides, {}));
}

bool PresentationView::ApplyHeaderFooter(const HeaderFooterSettings& s, const std::vector<size_t>& slides) {
  if (!CanEditSlides()) return false;
  edit::HeaderFooter hf;
  hf.slideNumber = s.slideNumber;
  hf.footer = s.footer;
  hf.footerText = s.footerText;
  hf.date = s.date;
  hf.dateFixed = !s.dateAuto;
  hf.dateText = s.dateFixed;
  hf.dateFormat = "datetime" + std::to_string(std::clamp(s.dateFormat, 1, 13));
  hf.notOnTitle = s.notOnTitle;
  return Done(SlideEditor()->SetHeaderFooter(slides, hf));
}

bool PresentationView::SetSlideSize(int choice, int scale) {
  const auto& sizes = SlideSizes();
  if (choice < 0 || static_cast<size_t>(choice) >= sizes.size() || !CanEditSlides()) return false;
  const draw::Size to = sizes[static_cast<size_t>(choice)].size, from = deck_->SlideSize();
  if (std::abs(to.width - from.width) < 0.5f && std::abs(to.height - from.height) < 0.5f) return true;
  // Larger: the content keeps its size, centered (PowerPoint doesn't ask then).
  const edit::Scale how = AsksHowToScale(from, to) && scale == static_cast<int>(ContentScale::Maximize) ? edit::Scale::Maximize
                                                                                                       : edit::Scale::EnsureFit;
  return Done(SlideEditor()->SetSlideSize(to, how));
}

bool PresentationView::SetNotes(size_t slide, const std::wstring& text) {
  if (slide >= SlideCount() || !CanEditSlides()) return false;
  return Done(SlideEditor()->SetNotesText(slide, text));
}

}  // namespace fw::slide::view
