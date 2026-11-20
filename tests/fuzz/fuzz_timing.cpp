// Fuzz target: the slide show's timing (src/slide/show/Timing.h, P1): a slide part, or a <p:timing>
// or <p:transition> on its own, read as the show reads it, then every step played through the
// animator, at a spread of times, with the targets' poses checked. Input that isn't XML is read as a
// timing formula. Seeds: tests/fuzz/seeds/timing (slide parts PowerPoint wrote, tests/office/show).
//
// Poses must stay finite, opacity and the shown part of a reveal within [0, 1], and the show must
// reach the slide's end in no more clicks than it has steps.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "pkg/XmlDom.h"
#include "slide/show/Timing.h"

namespace {

using namespace fw::slide::show;

void Require(bool ok) {
  if (!ok) std::abort();
}

TargetBox Box(void*, const TargetKey& k) {
  double f = (k.spid % 7) / 8.0;
  return {0.1 + f, 0.9 - f, 0.05 + f / 2, 0.3 - f / 4};
}

void CheckPoses(const Animator& a) {
  for (const auto& [key, p] : a.Poses()) {
    Require(std::isfinite(p.opacity) && p.opacity >= 0 && p.opacity <= 1);
    Require(std::isfinite(p.dx) && std::isfinite(p.dy) && std::isfinite(p.sx) && std::isfinite(p.sy) &&
            std::isfinite(p.rotation));
    Require(std::isfinite(p.reveal.shown) && p.reveal.shown >= 0 && p.reveal.shown <= 1);
    (void)key;
  }
}

pugi::xml_node Find(pugi::xml_node root, std::string_view local) {
  if (fw::pkg::LocalNameOf(root) == local) return root;
  for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling())
    if (c.type() == pugi::node_element && fw::pkg::LocalNameOf(c) == local) return c;
  return {};
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view text(reinterpret_cast<const char*>(data), size);
  fw::pkg::XmlDom dom;
  if (!dom.Parse(text).Ok()) {
    double v = EvalFormula(text, TargetBox{0.5, 0.5, 0.25, 0.25}, 0.5);
    Require(std::isnan(v) || std::isfinite(v));
    return 0;
  }
  pugi::xml_node root = dom.Root();
  pugi::xml_node trans = Find(root, "transition");
  if (!trans) trans = Find(root, "AlternateContent");
  Transition t = ParseTransition(trans);
  Require(t.durationMs >= 0 && t.durationMs <= 60000 && std::isfinite(t.durationMs));

  Timing timing = ParseTiming(Find(root, "timing"));
  for (const Step& s : timing.main.steps) Require(std::isfinite(s.length) && s.length >= 0);

  Animator a(&timing, Box, nullptr);
  double now = 0;
  a.Start(now);
  CheckPoses(a);
  size_t clicks = 0;
  for (;;) {
    for (int i = 1; i <= 4; ++i) {
      a.Update(now + i * 137.0);
      CheckPoses(a);
    }
    now += 1000;
    if (!a.Click(now)) break;
    Require(++clicks <= timing.main.steps.size());
    CheckPoses(a);
  }
  for (const Sequence& s : timing.triggers) {
    a.Trigger(s.triggerSpid, now);
    a.Update(now + 250);
    CheckPoses(a);
  }
  while (a.Back()) CheckPoses(a);
  a.FinishAll(now);
  CheckPoses(a);
  Require(!a.HasNextStep());
  return 0;
}
