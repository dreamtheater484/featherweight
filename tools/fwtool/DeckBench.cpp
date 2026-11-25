// fwtool bench --decks: P2's editing budgets (plan §11, docs/p2-plan.md) on a deck, through the edit
// API (the torture test's DeckTarget, DeckTorture.h), each timed from the edit to the slide drawn
// again as the slide view shows it (SlideRender at 96 dpi):
//   - a keystroke in a text box or placeholder: < 16 ms typical (the median of letters), < 50 ms worst
//     (every key, Enter included);
//   - a slide operation (new, duplicate, delete, move) on a 50-slide deck: < 50 ms each (the worst run);
//   - undo of each: < 50 ms (the worst).
// Saving is timed too, for the record (the app saves on a worker thread). Without the edit API in
// the build every edit is refused and the bench fails with that reason.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Common.h"
#include "DeckTorture.h"
#include "core/Strings.h"
#include "slide/Presentation.h"
#include "slide/render/SlideRender.h"
#include "text/Fonts.h"

namespace fwtool {
namespace {

namespace dt = decktorture;
namespace sl = fw::slide;

constexpr double kTypingBudget = 16, kTypingWorstBudget = 50, kSlideOpBudget = 50, kUndoBudget = 50;  // ms

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

double Worst(const std::vector<double>& v) { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }

struct Bench {
  std::unique_ptr<dt::DeckTarget> target;
  fw::text::Fonts fonts;
  std::string refused;  // why the edit API refused an edit (the bench can't measure then)

  // The slide drawn again as the slide view shows it: a slide's worth of pixels at 96 dpi.
  void Show(size_t index) {
    sl::Presentation& deck = target->Deck();
    if (index >= deck.SlideCount()) return;
    auto render = fw::slide::render::SlideRender::Make(deck, *deck.GetSlide(index), fonts);
    const draw::Size size = render->Size();
    fw::slide::render::RenderBitmap(*render, static_cast<int>(size.width * 4 / 3), static_cast<int>(size.height * 4 / 3));
  }
  // Applies an edit and draws `show` again; the milliseconds, or -1 when the edit was refused.
  double Timed(const dt::Edit& e, size_t show) {
    std::string why;
    const double t0 = Now();
    if (target->Apply(e, why) != dt::Applied::Done) {
      if (refused.empty()) refused = dt::FormatEdit(e) + ": " + why;
      return -1;
    }
    Show(show);
    return Now() - t0;
  }
};

// A shape to type into: the slide's first placeholder or text box with text, else any shape with a
// text body.
const sl::Shape* TypingTarget(const sl::Slide& s) {
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

std::string JsonText(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (static_cast<unsigned char>(c) >= 0x20) out += c;
  }
  return out + "\"";
}

std::string Ms(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return buf;
}

}  // namespace

int RunDeckEditBench(const Options& options) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int runs = std::max(1, options.runs);
  std::string json = "{\n  \"budgets\": {\"typingMs\": " + Ms(kTypingBudget) + ", \"typingWorstMs\": " + Ms(kTypingWorstBudget) +
                     ", \"slideOpMs\": " + Ms(kSlideOpBudget) + ", \"undoMs\": " + Ms(kUndoBudget) + "},\n  \"files\": [";
  int failed = 0;
  bool firstFile = true;
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
    deck->LoadAll();
    Bench b;
    fw::slide::render::UseDeckFonts(b.fonts, *deck);
    b.target = dt::OpenDeckTarget(std::move(deck), st);
    sl::Presentation& d = b.target->Deck();
    const size_t slides = d.SlideCount();
    if (slides < 2) {
      std::printf("  needs two slides or more\n");
      ++failed;
      continue;
    }
    const size_t at = std::min<size_t>(10, slides - 1);  // the slide the operations start from
    b.Show(at);  // warm: fonts and pictures loaded, as when the deck is open
    std::vector<std::string> failures;

    // Typing: letters and spaces at the end of a text, an Enter every 12 keys.
    std::vector<double> letters, keys;
    size_t typeSlide = at;
    const sl::Shape* shape = TypingTarget(*d.GetSlide(at));
    for (size_t i = 0; !shape && i < slides; ++i) {
      shape = TypingTarget(*d.GetSlide(i));
      if (shape) typeSlide = i;
    }
    if (!shape) {
      failures.push_back("no shape with text to type into");
    } else {
      const uint32_t id = shape->id;
      const char* typed = "the quick brown fox jumps over the lazy dog ";
      for (int r = 0; r < runs && b.refused.empty(); ++r) {
        for (int k = 0; k < 24 && b.refused.empty(); ++k) {
          dt::Edit e;
          e.op = dt::Op::Type;
          e.slide = static_cast<int>(typeSlide);
          e.shape = id;
          const bool enter = k % 12 == 11;
          e.arg = enter ? "para" : "end";
          e.text = enter ? "" : std::string(1, typed[(r * 24 + k) % 44]);
          const double ms = b.Timed(e, typeSlide);
          if (ms < 0) break;
          keys.push_back(ms);
          if (!enter) letters.push_back(ms);
        }
      }
    }
    const double typical = Median(letters), worstKey = Worst(keys);

    // Slide operations, each followed by its undo, on the 50-slide deck.
    std::map<std::string, std::vector<double>> ops;
    std::vector<double> undos;
    auto slideOp = [&](const char* name, dt::Edit e, size_t show) {
      const double ms = b.Timed(e, show);
      if (ms < 0) return;
      ops[name].push_back(ms);
      dt::Edit u;
      u.op = dt::Op::Undo;
      const double undo = b.Timed(u, at);
      if (undo >= 0) undos.push_back(undo);
    };
    uint32_t layout = 0;
    for (uint32_t l = 0; l < d.Layouts().size(); ++l)
      if (d.Layouts()[l].get() == d.GetSlide(at)->layout) layout = l;
    for (int r = 0; r < runs && b.refused.empty(); ++r) {
      dt::Edit e;
      e.op = dt::Op::NewSlide, e.slide = static_cast<int>(at), e.count = layout;
      slideOp("new", e, at + 1);
      e = {}, e.op = dt::Op::Duplicate, e.slide = static_cast<int>(at);
      slideOp("duplicate", e, at + 1);
      e = {}, e.op = dt::Op::DeleteSlide, e.slide = static_cast<int>(at);
      slideOp("delete", e, at);
      e = {}, e.op = dt::Op::MoveSlide, e.slide = static_cast<int>(at), e.to = static_cast<int>(slides - 1);
      slideOp("move", e, slides - 1);
    }
    // Saving, for the record.
    double saveMs = -1;
    if (b.refused.empty()) {
      std::vector<uint8_t> bytes;
      const double t0 = Now();
      if (b.target->Save(bytes).Ok()) saveMs = Now() - t0;
    }

    if (!b.refused.empty()) failures.push_back("the edit API refused " + b.refused + " (edits through: " + b.target->Name() + ")");
    std::printf("  %zu slides; typing in slide %zu: %.2f ms typical, %.2f ms worst (%zu keys)\n", slides, typeSlide + 1, typical, worstKey,
                keys.size());
    if (!keys.empty() && typical >= kTypingBudget) failures.push_back("typing: " + Ms(typical) + " ms typical, over " + Ms(kTypingBudget));
    if (!keys.empty() && worstKey >= kTypingWorstBudget) failures.push_back("typing: the worst key " + Ms(worstKey) + " ms, over " + Ms(kTypingWorstBudget));
    std::string opsJson;
    for (const auto& [name, times] : ops) {
      std::printf("  %-9s %.2f ms median, %.2f ms worst\n", name.c_str(), Median(times), Worst(times));
      if (Worst(times) >= kSlideOpBudget) failures.push_back(name + " slide: " + Ms(Worst(times)) + " ms, over " + Ms(kSlideOpBudget));
      opsJson += std::string(opsJson.empty() ? "" : ", ") + "\"" + name + "\": {\"medianMs\": " + Ms(Median(times)) +
                 ", \"worstMs\": " + Ms(Worst(times)) + "}";
    }
    std::printf("  undo      %.2f ms median, %.2f ms worst\n", Median(undos), Worst(undos));
    if (Worst(undos) >= kUndoBudget) failures.push_back("undo: " + Ms(Worst(undos)) + " ms, over " + Ms(kUndoBudget));
    if (saveMs >= 0) std::printf("  save      %.1f ms (on a worker thread in the app)\n", saveMs);
    for (const std::string& f : failures) std::printf("  FAIL: %s\n", f.c_str());
    if (!failures.empty() && (options.budgets || !b.refused.empty())) ++failed;

    std::string failuresJson;
    for (const std::string& f : failures) failuresJson += std::string(failuresJson.empty() ? "" : ", ") + JsonText(f);
    json += std::string(firstFile ? "" : ",") + "\n    {\"name\": " + JsonText(fw::WideToUtf8(file.filename().wstring())) +
            ", \"slides\": " + std::to_string(slides) + ", \"typing\": {\"typicalMs\": " + Ms(typical) + ", \"worstMs\": " +
            Ms(worstKey) + ", \"keys\": " + std::to_string(keys.size()) + "}, \"ops\": {" + opsJson + "}, \"undo\": {\"medianMs\": " +
            Ms(Median(undos)) + ", \"worstMs\": " + Ms(Worst(undos)) + "}, \"saveMs\": " + (saveMs >= 0 ? Ms(saveMs) : "null") +
            ", \"failures\": [" + failuresJson + "]}";
    firstFile = false;
  }
  json += "\n  ]\n}\n";
  if (!options.jsonOut.empty()) std::ofstream(options.jsonOut, std::ios::trunc) << json;
  return failed ? 1 : 0;
}

}  // namespace fwtool
