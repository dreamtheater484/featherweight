// fwtool bench --slide-text: keystroke to screen in the presentation tab's text editing (P2, p2-text;
// plan §11: < 16 ms typical, < 50 ms worst, the body alone laid out again). Types through what the
// tab uses (TextSession over EditTextModel over p2-model's Editor) at the end of a text on slide 11,
// or the first slide with one, each key timed from the keystroke to the edited text drawn on the
// graphics adapter as the tab draws it over the slide's bitmap (kept from before), at 100 % on a 96 dpi
// screen: the working copy edited, the body laid out, the edit committed to the model (the typing one
// undo step) and the text drawn with its outline. An Enter every 12 keys; the worst is over every key.
#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "draw/D2DDevice.h"
#include "draw/D2DPainter.h"
#include "pkg/Package.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/render/SlideRender.h"
#include "slide/view/TextModelEdit.h"
#include "slide/view/TextSession.h"
#include "text/Fonts.h"

namespace fwtool {
bool IsDeckFile(const std::filesystem::path& file);

namespace {

namespace sl = fw::slide;
using Microsoft::WRL::ComPtr;

constexpr double kTypical = 16, kWorst = 50;  // ms

double Now() {
  LARGE_INTEGER t, f;
  QueryPerformanceCounter(&t);
  QueryPerformanceFrequency(&f);
  return t.QuadPart * 1000.0 / f.QuadPart;
}

double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

std::string JsonText(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}

std::string Ms(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return buf;
}

// The tab's drawing of the edited text: the slide's bitmap (made once, as the tab keeps it) and the
// text over it, on the graphics adapter.
class Screen {
 public:
  bool Init(draw::Size slide) {
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
      return false;
    if (FAILED(draw::CreateD2DDevice(factory_.Get(), false, d3d_, d2d_)) &&
        FAILED(draw::CreateD2DDevice(factory_.Get(), true, d3d_, d2d_)))
      return false;
    if (FAILED(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    scale_ = 96.0f / 72.0f;
    const D2D1_SIZE_U size{static_cast<UINT32>(slide.width * scale_ + 1), static_cast<UINT32>(slide.height * scale_ + 1)};
    const D2D1_BITMAP_PROPERTIES1 target = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, target, &target_))) return false;
    const D2D1_BITMAP_PROPERTIES1 plain =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, plain, &slide_))) return false;
    dc_->SetTarget(target_.Get());
    return true;
  }
  // The slide without the edited text, as the tab's bitmap.
  void Prepare(sl::render::SlideRender& r, sl::render::DrawFilter* hide) {
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(1, 1, 1, 1));
    {
      draw::D2DPainter painter(dc_.Get());
      painter.Transform(draw::Matrix::Scale(scale_, scale_));
      const draw::Size s = r.Size();
      r.Draw(painter, {0, 0, s.width, s.height}, hide);
    }
    dc_->EndDraw();
    const D2D1_POINT_2U at{0, 0};
    const D2D1_RECT_U all{0, 0, target_->GetPixelSize().width, target_->GetPixelSize().height};
    slide_->CopyFromBitmap(&at, target_.Get(), &all);
  }
  void Draw(const sl::view::TextSession& s) {
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    dc_->DrawBitmap(slide_.Get());
    {
      draw::D2DPainter painter(dc_.Get());
      const draw::Matrix toScreen = draw::Matrix::Scale(scale_, scale_);
      const sl::view::TextFrame& f = s.Frame();
      const draw::Matrix m = f.toSlide * toScreen;
      dc_->SetTransform(D2D1::Matrix3x2F(m.m11, m.m12, m.m21, m.m22, m.dx, m.dy));
      s.Block().Draw(painter, nullptr, s.Target().shape);
    }
    dc_->EndDraw();
  }

 private:
  ComPtr<ID2D1Factory1> factory_;
  ComPtr<ID3D11Device> d3d_;
  ComPtr<ID2D1Device> d2d_;
  ComPtr<ID2D1DeviceContext> dc_;
  ComPtr<ID2D1Bitmap1> target_, slide_;
  float scale_ = 1;
};

struct HideShape : sl::render::DrawFilter {
  uint32_t shape = 0;
  bool Paragraph(uint32_t id, int) override { return id != shape; }
};

// The text to type into: a placeholder or text box with text (not a title), else any shape's text.
const sl::Shape* Target(const sl::Slide& s) {
  const sl::Shape* any = nullptr;
  for (const sl::Shape& sh : s.shapes) {
    if (sh.kind != sl::Shape::Kind::Shape || !sh.textNode) continue;
    if (sh.textBox || (sh.placeholder && sh.placeholder.type != sl::PlaceholderType::Title &&
                       sh.placeholder.type != sl::PlaceholderType::CenteredTitle))
      return &sh;
    if (!any) any = &sh;
  }
  return any;
}

}  // namespace

int RunSlideTextBench(const Options& options) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int runs = std::max(1, options.runs);
  std::printf("Typing in a slide's text as the presentation tab does: %d x 24 keys per deck, ms from the key to the text drawn.\n", runs);
  std::string json = "{\n  \"budgets\": {\"typingMs\": " + Ms(kTypical) + ", \"typingWorstMs\": " + Ms(kWorst) + "},\n  \"files\": [";
  int failed = 0;
  bool first = true;
  for (const auto& file : CollectFiles(options.inputs)) {
    if (!IsDeckFile(file)) continue;
    std::printf("%s\n", Display(file).c_str());
    fw::pkg::Status st;
    auto pkg = OpenPackage(file, st);
    auto deck = pkg ? sl::Read(std::move(pkg), st) : nullptr;
    if (!deck) {
      std::printf("  refused: %s\n", st.message.c_str());
      ++failed;
      continue;
    }
    fw::text::Fonts fonts;
    sl::render::UseDeckFonts(fonts, *deck);
    sl::edit::Editor editor(*deck);
    sl::view::EditTextModel model(*deck, editor);
    size_t at = std::min<size_t>(10, deck->SlideCount() - 1);
    const sl::Shape* shape = deck->SlideCount() ? Target(*deck->GetSlide(at)) : nullptr;
    for (size_t i = 0; !shape && i < deck->SlideCount(); ++i) {
      shape = Target(*deck->GetSlide(i));
      if (shape) at = i;
    }
    if (!shape) {
      std::printf("  no text to type into\n");
      ++failed;
      continue;
    }
    sl::view::TextTarget t;
    t.slide = at;
    t.shape = shape->id;
    auto render = sl::render::SlideRender::Make(*deck, *deck->GetSlide(at), fonts);
    sl::render::SlideRender::TextFrame frame;
    Screen screen;
    if (!render->FindText(t.shape, -1, -1, frame) || !screen.Init(render->Size())) {
      std::printf("  can't lay the text out or draw\n");
      ++failed;
      continue;
    }
    HideShape hide;
    hide.shape = t.shape;
    screen.Prepare(*render, &hide);
    sl::view::TextSession session(model, t, fonts);
    if (!session.Start(frame, {})) {
      std::printf("  can't edit that text\n");
      ++failed;
      continue;
    }
    session.MoveCaret(sl::view::TextSession::Move::End, false);
    screen.Draw(session);  // warm
    std::vector<double> letters, keys, layouts;
    std::string refused;
    const char* typed = "the quick brown fox jumps over the lazy dog ";
    for (int r = 0; r < runs && refused.empty(); ++r) {
      for (int k = 0; k < 24; ++k) {
        const bool enter = k % 12 == 11;
        const double t0 = Now();
        const bool ok = enter ? session.Enter() : session.Type(std::wstring(1, static_cast<wchar_t>(typed[(r * 24 + k) % 44])));
        if (!ok) {
          refused = fw::WideToUtf8(session.TakeError());
          break;
        }
        if (session.TakeGeometryChanged()) {
          // The shape grew to its text (or a row did): as the tab, the slide is drawn again and the
          // text laid out in its new place.
          render = sl::render::SlideRender::Make(*deck, *deck->GetSlide(at), fonts);
          if (render->FindText(t.shape, -1, -1, frame)) session.SetFrame(frame);
          screen.Prepare(*render, &hide);
        }
        screen.Draw(session);
        const double ms = Now() - t0;
        keys.push_back(ms);
        layouts.push_back(session.LastLayoutMs());
        if (!enter) letters.push_back(ms);
      }
    }
    const double typical = Median(letters), worst = keys.empty() ? 0 : *std::max_element(keys.begin(), keys.end());
    std::printf("  slide %zu, shape %u: %.2f ms typical, %.2f ms worst (%zu keys; layout %.2f ms typical); %zu undo steps\n", at + 1,
                t.shape, typical, worst, keys.size(), Median(layouts), editor.UndoNames().size());
    std::vector<std::string> failures;
    if (!refused.empty()) failures.push_back("the edit was refused: " + refused);
    if (typical >= kTypical) failures.push_back("typing: " + Ms(typical) + " ms typical, over " + Ms(kTypical));
    if (worst >= kWorst) failures.push_back("typing: the worst key " + Ms(worst) + " ms, over " + Ms(kWorst));
    for (const std::string& f : failures) std::printf("  over budget: %s\n", f.c_str());
    if (!failures.empty() && options.budgets) ++failed;
    json += std::string(first ? "\n" : ",\n") + "    {\"file\": " + JsonText(Display(file)) + ", \"slide\": " + std::to_string(at + 1) +
            ", \"typicalMs\": " + Ms(typical) + ", \"worstMs\": " + Ms(worst) + ", \"layoutMs\": " + Ms(Median(layouts)) +
            ", \"keys\": " + std::to_string(keys.size()) + ", \"undoSteps\": " + std::to_string(editor.UndoNames().size()) + "}";
    first = false;
  }
  json += "\n  ]\n}\n";
  if (!options.jsonOut.empty()) std::ofstream(options.jsonOut, std::ios::binary) << json;
  return failed ? 1 : 0;
}

}  // namespace fwtool
