// The slide show's transitions, timing tree and animation interpreter (src/slide/show/Timing.h).
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "pkg/XmlDom.h"
#include "slide/Presentation.h"
#include "slide/show/Timing.h"

using namespace fw::slide::show;

namespace {

const char* kNs =
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\""
    " xmlns:p14=\"http://schemas.microsoft.com/office/powerpoint/2010/main\""
    " xmlns:p159=\"http://schemas.microsoft.com/office/powerpoint/2015/09/main\"";

// A parsed snippet whose root element gets the namespaces above.
struct Doc {
  fw::pkg::XmlDom dom;
  explicit Doc(const std::string& xml) {
    std::string s = xml;
    size_t end = s.find_first_of(" >/", 1);
    s.insert(end, kNs);
    CHECK(dom.Parse(s).Ok());
  }
  pugi::xml_node Root() const { return dom.Root(); }
};

Transition Trans(const std::string& xml) {
  Doc d(xml);
  return ParseTransition(d.Root());
}

// ---- The timing tree as PowerPoint writes it ----

std::string Tgt(int spid, int para = -1) {
  std::string s = "<p:tgtEl><p:spTgt spid=\"" + std::to_string(spid) + "\">";
  if (para >= 0)
    s += "<p:txEl><p:pRg st=\"" + std::to_string(para) + "\" end=\"" + std::to_string(para) + "\"/></p:txEl>";
  return s + "</p:spTgt></p:tgtEl>";
}

std::string Set(int spid, const char* value, int delay = 0, int para = -1) {
  return "<p:set><p:cBhvr><p:cTn id=\"9\" dur=\"1\" fill=\"hold\"><p:stCondLst><p:cond delay=\"" +
         std::to_string(delay) + "\"/></p:stCondLst></p:cTn>" + Tgt(spid, para) +
         "<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst></p:cBhvr><p:to><p:strVal val=\"" +
         value + "\"/></p:to></p:set>";
}

std::string Anim(int spid, const char* attr, int dur, const char* from, const char* to, const char* extra = "") {
  return std::string("<p:anim calcmode=\"lin\" valueType=\"num\"><p:cBhvr additive=\"base\"><p:cTn id=\"9\" dur=\"") +
         std::to_string(dur) + "\" fill=\"hold\"" + extra + "/>" + Tgt(spid) + "<p:attrNameLst><p:attrName>" + attr +
         "</p:attrName></p:attrNameLst></p:cBhvr><p:tavLst><p:tav tm=\"0\"><p:val><p:strVal val=\"" + from +
         "\"/></p:val></p:tav><p:tav tm=\"100000\"><p:val><p:strVal val=\"" + to +
         "\"/></p:val></p:tav></p:tavLst></p:anim>";
}

std::string Filter(int spid, const char* transition, const char* filter, int dur, int para = -1) {
  return std::string("<p:animEffect transition=\"") + transition + "\" filter=\"" + filter + "\"><p:cBhvr><p:cTn id=\"9\" dur=\"" +
         std::to_string(dur) + "\"/>" + Tgt(spid, para) + "</p:cBhvr></p:animEffect>";
}

std::string Effect(const char* cls, int preset, const char* nodeType, int delay, const std::string& behaviors,
                   const char* extra = "") {
  return std::string("<p:par><p:cTn id=\"5\" presetID=\"") + std::to_string(preset) + "\" presetClass=\"" + cls +
         "\" presetSubtype=\"0\" fill=\"hold\" nodeType=\"" + nodeType + "\"" + extra + "><p:stCondLst><p:cond delay=\"" +
         std::to_string(delay) + "\"/></p:stCondLst><p:childTnLst>" + behaviors + "</p:childTnLst></p:cTn></p:par>";
}

// An "after previous" chain within a click group starts at `delay` (PowerPoint writes the time).
std::string Sub(int delay, const std::string& effects) {
  return "<p:par><p:cTn id=\"4\" fill=\"hold\"><p:stCondLst><p:cond delay=\"" + std::to_string(delay) +
         "\"/></p:stCondLst><p:childTnLst>" + effects + "</p:childTnLst></p:cTn></p:par>";
}

// A click group: waits for a click, or (auto) starts with the slide.
std::string Group(const std::string& subs, bool autoStart = false) {
  std::string cond = autoStart ? "<p:cond delay=\"indefinite\"/><p:cond evt=\"onBegin\" delay=\"0\"><p:tn val=\"2\"/></p:cond>"
                               : "<p:cond delay=\"indefinite\"/>";
  return "<p:par><p:cTn id=\"3\" fill=\"hold\"><p:stCondLst>" + cond + "</p:stCondLst><p:childTnLst>" + subs +
         "</p:childTnLst></p:cTn></p:par>";
}

std::string TimingXml(const std::string& groups, const std::string& more = "", const std::string& bld = "") {
  return "<p:timing><p:tnLst><p:par><p:cTn id=\"1\" dur=\"indefinite\" restart=\"never\" nodeType=\"tmRoot\"><p:childTnLst>"
         "<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=\"2\" dur=\"indefinite\" nodeType=\"mainSeq\"><p:childTnLst>" +
         groups +
         "</p:childTnLst></p:cTn><p:prevCondLst><p:cond evt=\"onPrev\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond>"
         "</p:prevCondLst><p:nextCondLst><p:cond evt=\"onNext\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond>"
         "</p:nextCondLst></p:seq>" +
         more + "</p:childTnLst></p:cTn></p:par></p:tnLst>" + (bld.empty() ? "" : "<p:bldLst>" + bld + "</p:bldLst>") +
         "</p:timing>";
}

Timing ParseXml(const std::string& xml) {
  Doc d(xml);
  return ParseTiming(d.Root());
}

TargetBox BoxOf(void*, const TargetKey& k) {
  if (k.spid == 4) return {0.5, 0.5, 0.2, 0.1};
  return {0.25, 0.25, 0.1, 0.1};
}

TargetKey Shape(uint32_t spid) { return {spid, TargetKey::Whole, 0, 0}; }
TargetKey Para(uint32_t spid, int p) { return {spid, TargetKey::Paragraphs, p, p}; }

std::string FadeIn(int spid, int dur = 500, int para = -1) {
  return Set(spid, "visible", 0, para) + Filter(spid, "in", "fade", dur, para);
}

}  // namespace

TEST(SlideShowTransitions) {
  Transition t = Trans("<p:transition spd=\"med\"><p:push dir=\"u\"/></p:transition>");
  CHECK(t.kind == TransitionKind::Push);
  CHECK(t.side == Side::Up);
  CHECK_NEAR(t.durationMs, 750, 0.01);
  CHECK(t.advanceOnClick);
  CHECK(t.advanceAfterMs < 0);

  t = Trans("<p:transition advClick=\"0\" advTm=\"3000\"><p:wipe/></p:transition>");
  CHECK(t.kind == TransitionKind::Wipe);
  CHECK(t.side == Side::Left);
  CHECK_NEAR(t.durationMs, 500, 0.01);
  CHECK(!t.advanceOnClick);
  CHECK_NEAR(t.advanceAfterMs, 3000, 0.01);

  t = Trans("<p:transition spd=\"slow\"><p:split orient=\"vert\" dir=\"in\"/></p:transition>");
  CHECK(t.kind == TransitionKind::Split && t.vertical && t.in);
  CHECK_NEAR(t.durationMs, 1000, 0.01);
  t = Trans("<p:transition><p:split/></p:transition>");
  CHECK(t.kind == TransitionKind::Split && !t.vertical && !t.in);
  t = Trans("<p:transition><p:cover dir=\"rd\"/></p:transition>");
  CHECK(t.kind == TransitionKind::Cover && t.side == Side::RightDown);
  t = Trans("<p:transition><p:pull dir=\"u\"/></p:transition>");
  CHECK(t.kind == TransitionKind::Uncover && t.side == Side::Up);
  t = Trans("<p:transition><p:zoom dir=\"in\"/></p:transition>");
  CHECK(t.kind == TransitionKind::Zoom && t.in);
  t = Trans("<p:transition><p:fade thruBlk=\"1\"/></p:transition>");
  CHECK(t.kind == TransitionKind::Fade && t.throughBlack && t.played);
  t = Trans("<p:transition><p:cut/></p:transition>");
  CHECK(t.kind == TransitionKind::None && t.durationMs == 0);
  t = Trans("<p:transition advTm=\"2000\"/>");
  CHECK(t.kind == TransitionKind::None);
  CHECK_NEAR(t.advanceAfterMs, 2000, 0.01);
  // Others play as a fade.
  t = Trans("<p:transition><p:checker/></p:transition>");
  CHECK(t.kind == TransitionKind::Fade && !t.played);

  // PowerPoint 2010 and later: a p14 Choice with the duration in ms.
  t = Trans(
      "<mc:AlternateContent><mc:Choice Requires=\"p14\"><p:transition spd=\"slow\" p14:dur=\"2000\"><p14:vortex dir=\"r\"/>"
      "</p:transition></mc:Choice><mc:Fallback><p:transition spd=\"slow\"><p:fade/></p:transition></mc:Fallback>"
      "</mc:AlternateContent>");
  CHECK(t.kind == TransitionKind::Fade && !t.played);
  CHECK_NEAR(t.durationMs, 2000, 0.01);
  t = Trans(
      "<mc:AlternateContent><mc:Choice Requires=\"p14\"><p:transition spd=\"med\" p14:dur=\"1250\"><p:push dir=\"d\"/>"
      "</p:transition></mc:Choice><mc:Fallback><p:transition spd=\"med\"><p:push dir=\"d\"/></p:transition></mc:Fallback>"
      "</mc:AlternateContent>");
  CHECK(t.kind == TransitionKind::Push && t.side == Side::Down);
  CHECK_NEAR(t.durationMs, 1250, 0.01);
  // Morph isn't read: its Fallback is.
  t = Trans(
      "<mc:AlternateContent><mc:Choice Requires=\"p159\"><p:transition spd=\"slow\" p14:dur=\"2000\"><p159:morph option=\"byObject\"/>"
      "</p:transition></mc:Choice><mc:Fallback><p:transition spd=\"slow\"><p:fade/></p:transition></mc:Fallback>"
      "</mc:AlternateContent>");
  CHECK(t.kind == TransitionKind::Fade && t.played);
  CHECK_NEAR(t.durationMs, 1000, 0.01);

  t = ParseTransition(pugi::xml_node());
  CHECK(t.kind == TransitionKind::None);
}

TEST(SlideShowFormulas) {
  TargetBox box{0.3, 0.6, 0.2, 0.4};
  CHECK_NEAR(EvalFormula("#ppt_x", box), 0.3, 1e-9);
  CHECK_NEAR(EvalFormula("0-#ppt_w/2", box), -0.1, 1e-9);
  CHECK_NEAR(EvalFormula("1+#ppt_h/2", box), 1.2, 1e-9);
  CHECK_NEAR(EvalFormula("#ppt_y+.1", box), 0.7, 1e-9);
  CHECK_NEAR(EvalFormula("ppt_x", box), 0.3, 1e-9);
  CHECK_NEAR(EvalFormula("sin(pi/2)", box), 1, 1e-9);
  CHECK_NEAR(EvalFormula("$*2", box, 3), 6, 1e-9);
  CHECK_NEAR(EvalFormula("max(1, 2) + min(3,4)", box), 5, 1e-9);
  CHECK_NEAR(EvalFormula("2^3^2", box), 512, 1e-9);
  CHECK_NEAR(EvalFormula("-.5", box), -0.5, 1e-9);
  CHECK_NEAR(EvalFormula("1.5e2", box), 150, 1e-9);
  CHECK_NEAR(EvalFormula(" ( 1 + 2 ) * 3 ", box), 9, 1e-9);
  CHECK_NEAR(EvalFormula("1/0", box), 0, 1e-9);
  CHECK(std::isnan(EvalFormula("#foo", box)));
  CHECK(std::isnan(EvalFormula("(1", box)));
  CHECK(std::isnan(EvalFormula("1 2", box)));
  CHECK(std::isnan(EvalFormula("visible", box)));
  CHECK(std::isnan(EvalFormula("", box)));
  CHECK(std::isnan(EvalFormula(std::string(500, '('), box)));
}

TEST(SlideShowFlyIn) {
  // Fly in from the bottom, on click, 0.5 s (presetID 2, subtype 4).
  Timing t = ParseXml(TimingXml(Group(Sub(0, Effect("entr", 2, "clickEffect", 0,
      Set(4, "visible") + Anim(4, "ppt_x", 500, "#ppt_x", "#ppt_x") + Anim(4, "ppt_y", 500, "1+#ppt_h/2", "#ppt_y")))),
      "", "<p:bldP spid=\"4\" grpId=\"0\" animBg=\"1\"/>"));
  CHECK_EQ(t.main.steps.size(), 1u);
  CHECK(t.main.steps[0].start == StepStart::Click);
  CHECK_NEAR(t.main.steps[0].length, 500, 0.01);
  CHECK_EQ(t.initiallyHidden.size(), 1u);
  CHECK(t.initiallyHidden[0] == Shape(4));
  CHECK_EQ(t.unsupportedEffects, 0);

  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  CHECK(!a.PoseOf(Shape(4)).visible);
  CHECK(!a.Running(0));
  CHECK(a.HasNextStep());
  CHECK(a.Click(1000));
  Pose p = a.PoseOf(Shape(4));
  CHECK(p.visible);
  CHECK_NEAR(p.dy, 0.55, 1e-6);  // 1 + 0.05 - 0.5: just below the slide
  CHECK_NEAR(p.dx, 0, 1e-6);
  a.Update(1250);
  CHECK(a.Running(1250));
  CHECK_EQ(a.Moving().size(), 1u);
  CHECK_NEAR(a.PoseOf(Shape(4)).dy, 0.275, 1e-6);
  a.Update(1500);
  CHECK(!a.Running(1500));
  CHECK(a.PoseOf(Shape(4)).Identity());
  CHECK(!a.HasNextStep());
  CHECK(!a.Click(2000));
}

TEST(SlideShowWithAndAfterPrevious) {
  // Shape 4 fades in on click; shape 5 with it after 0.2 s; shape 6 after the previous (at 0.5 s).
  Timing t = ParseXml(TimingXml(Group(
      Sub(0, Effect("entr", 10, "clickEffect", 0, FadeIn(4)) + Effect("entr", 10, "withEffect", 200, FadeIn(5))) +
      Sub(700, Effect("entr", 10, "afterEffect", 0, FadeIn(6, 1000))))));
  CHECK_EQ(t.main.steps.size(), 1u);
  CHECK_NEAR(t.main.steps[0].length, 1700, 0.01);
  CHECK_EQ(t.initiallyHidden.size(), 3u);
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  a.Click(0);
  a.Update(100);
  CHECK_NEAR(a.PoseOf(Shape(4)).opacity, 0.2, 1e-6);
  CHECK(!a.PoseOf(Shape(5)).visible);  // its delay hasn't passed
  a.Update(450);
  CHECK(a.PoseOf(Shape(5)).visible);
  CHECK_NEAR(a.PoseOf(Shape(5)).opacity, 0.5, 1e-6);
  CHECK(!a.PoseOf(Shape(6)).visible);
  a.Update(1200);
  CHECK(a.PoseOf(Shape(4)).Identity());
  CHECK_NEAR(a.PoseOf(Shape(6)).opacity, 0.5, 1e-6);
  a.Update(1700);
  CHECK(!a.Running(1700));
  CHECK(a.PoseOf(Shape(6)).Identity());
}

TEST(SlideShowAutoStartAndChain) {
  // The first group starts with the slide; the second waits for a click.
  Timing t = ParseXml(TimingXml(Group(Sub(0, Effect("entr", 10, "afterEffect", 1000, FadeIn(4))), true) +
                                Group(Sub(0, Effect("entr", 1, "clickEffect", 0, Set(5, "visible"))))));
  CHECK_EQ(t.main.steps.size(), 2u);
  CHECK(t.main.steps[0].start == StepStart::WithPrevious);
  CHECK(t.main.steps[1].start == StepStart::Click);
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  CHECK(a.Running(0));
  a.Update(500);
  CHECK(!a.PoseOf(Shape(4)).visible);  // after a 1 s delay
  a.Update(1250);
  CHECK_NEAR(a.PoseOf(Shape(4)).opacity, 0.5, 1e-6);
  a.Update(2000);
  CHECK(!a.Running(2000));
  CHECK(a.PoseOf(Shape(4)).Identity());
  CHECK(!a.PoseOf(Shape(5)).visible);
  // Appear: no time at all.
  CHECK(a.Click(3000));
  a.Update(3000.5);
  CHECK(a.PoseOf(Shape(5)).visible);
  a.Update(3002);
  CHECK(!a.Running(3002));

  // Back undoes the click; at the start there's nothing to undo.
  CHECK(a.Back());
  CHECK(!a.PoseOf(Shape(5)).visible);
  CHECK(a.PoseOf(Shape(4)).visible);
  CHECK(!a.Back());

  // Coming back from the next slide: everything built.
  a.FinishAll(0);
  CHECK(a.PoseOf(Shape(4)).Identity());
  CHECK(a.PoseOf(Shape(5)).Identity());
  CHECK(!a.HasNextStep());
  CHECK(a.Back());
  CHECK(!a.PoseOf(Shape(5)).visible);
}

TEST(SlideShowClickDuringStep) {
  // A click while a step runs finishes it and starts the next.
  Timing t = ParseXml(TimingXml(Group(Sub(0, Effect("entr", 10, "clickEffect", 0, FadeIn(4, 2000)))) +
                                Group(Sub(0, Effect("entr", 10, "clickEffect", 0, FadeIn(5, 2000))))));
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  a.Click(0);
  a.Update(500);
  CHECK_NEAR(a.PoseOf(Shape(4)).opacity, 0.25, 1e-6);
  CHECK(a.Click(600));
  CHECK(a.PoseOf(Shape(4)).Identity());
  a.Update(1600);
  CHECK_NEAR(a.PoseOf(Shape(5)).opacity, 0.5, 1e-6);
  a.Finish();
  CHECK(a.PoseOf(Shape(5)).Identity());
  CHECK(!a.Running(1600));
}

TEST(SlideShowByParagraphWipe) {
  // Wipe from the bottom (PowerPoint writes filter="wipe(down)"), by paragraph: one click per paragraph.
  auto wipe = [](int para) {
    return Group(Sub(0, Effect("entr", 22, "clickEffect", 0,
                               Set(3, "visible", 0, para) + Filter(3, "in", "wipe(down)", 500, para))));
  };
  Timing t = ParseXml(TimingXml(wipe(0) + wipe(1), "", "<p:bldP spid=\"3\" grpId=\"0\" build=\"p\"/>"));
  CHECK_EQ(t.main.steps.size(), 2u);
  CHECK_EQ(t.byParagraph.size(), 1u);
  CHECK_EQ(t.byParagraph[0], 3u);
  CHECK_EQ(t.initiallyHidden.size(), 2u);
  CHECK_EQ(t.targets.size(), 2u);
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  CHECK(!a.PoseOf(Para(3, 0)).visible);
  CHECK(!a.PoseOf(Para(3, 1)).visible);
  CHECK(a.PoseOf(Shape(3)).Identity());  // the shape itself isn't animated
  a.Click(0);
  a.Update(250);
  Pose p = a.PoseOf(Para(3, 0));
  CHECK(p.visible);
  CHECK(p.reveal.kind == Reveal::Wipe);
  CHECK(p.reveal.side == Side::Down);
  CHECK_NEAR(p.reveal.shown, 0.5, 1e-6);
  a.Update(500);
  CHECK(a.PoseOf(Para(3, 0)).Identity());
  CHECK(!a.PoseOf(Para(3, 1)).visible);
  a.Click(600);
  a.Update(1100);
  CHECK(a.PoseOf(Para(3, 1)).Identity());
}

TEST(SlideShowSplitZoomFloat) {
  // Split (barn), zoom (ppt_w and ppt_h from 0, with a fade) and float in (ppt_y from below, fade).
  Timing t = ParseXml(TimingXml(
      Group(Sub(0, Effect("entr", 16, "clickEffect", 0, Set(4, "visible") + Filter(4, "in", "barn(inVertical)", 500)))) +
      Group(Sub(0, Effect("entr", 53, "clickEffect", 0,
                          Set(5, "visible") + Anim(5, "ppt_w", 500, "0", "#ppt_w") + Anim(5, "ppt_h", 500, "0", "#ppt_h") +
                              Filter(5, "in", "fade", 500)))) +
      Group(Sub(0, Effect("entr", 42, "clickEffect", 0,
                          Set(6, "visible") + Filter(6, "in", "fade", 1000) +
                              Anim(6, "ppt_y", 1000, "#ppt_y+.1", "#ppt_y", " decel=\"100000\""))))));
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  a.Click(0);
  a.Update(250);
  Pose p = a.PoseOf(Shape(4));
  CHECK(p.reveal.kind == Reveal::Split && p.reveal.vertical && !p.reveal.outward);
  CHECK_NEAR(p.reveal.shown, 0.5, 1e-6);
  a.Click(1000);
  a.Update(1250);
  p = a.PoseOf(Shape(5));
  CHECK_NEAR(p.sx, 0.5, 1e-5);
  CHECK_NEAR(p.sy, 0.5, 1e-5);
  CHECK_NEAR(p.opacity, 0.5, 1e-6);
  a.Click(2000);
  a.Update(2500);
  p = a.PoseOf(Shape(6));
  // Full deceleration: at half the time, 3/4 of the way (SMIL's decel).
  CHECK_NEAR(p.dy, 0.1 * 0.25, 1e-5);
  a.Update(3000);
  CHECK(a.PoseOf(Shape(6)).Identity());
}

TEST(SlideShowExitAndEmphasis) {
  std::string spin = "<p:animRot by=\"21600000\"><p:cBhvr><p:cTn id=\"9\" dur=\"2000\" fill=\"hold\"/>" + Tgt(5) +
                     "<p:attrNameLst><p:attrName>r</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>";
  std::string grow = "<p:animScale><p:cBhvr><p:cTn id=\"9\" dur=\"2000\" fill=\"hold\"/>" + Tgt(4) +
                     "</p:cBhvr><p:by x=\"150000\" y=\"150000\"/></p:animScale>";
  std::string fadeOut = Filter(4, "out", "fade", 500) + Set(4, "hidden", 499);
  Timing t = ParseXml(TimingXml(Group(Sub(0, Effect("emph", 6, "clickEffect", 0, grow) +
                                              Effect("emph", 8, "withEffect", 0, spin))) +
                                Group(Sub(0, Effect("exit", 10, "clickEffect", 0, fadeOut)))));
  CHECK(t.initiallyHidden.empty());
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  CHECK(a.PoseOf(Shape(4)).Identity());
  a.Click(0);
  a.Update(1000);
  CHECK_NEAR(a.PoseOf(Shape(4)).sx, 1.25, 1e-5);
  CHECK_NEAR(a.PoseOf(Shape(5)).rotation, 180, 1e-3);
  a.Update(2000);
  CHECK_NEAR(a.PoseOf(Shape(4)).sx, 1.5, 1e-5);
  CHECK_NEAR(a.PoseOf(Shape(4)).sy, 1.5, 1e-5);
  CHECK_NEAR(a.PoseOf(Shape(5)).rotation, 360, 1e-3);
  a.Click(3000);
  a.Update(3250);
  Pose p = a.PoseOf(Shape(4));
  CHECK(p.visible);
  CHECK_NEAR(p.opacity, 0.5, 1e-6);
  CHECK_NEAR(p.sx, 1.5, 1e-5);  // the grow holds
  a.Update(3600);
  CHECK(!a.PoseOf(Shape(4)).visible);
  CHECK(!a.Running(3600));
}

TEST(SlideShowOthersPlayAsAppear) {
  // Blinds (presetID 3) and an unknown emphasis (teeter, 32): appear, and nothing.
  std::string teeter = "<p:animRot by=\"120000\"><p:cBhvr><p:cTn id=\"9\" dur=\"100\" fill=\"hold\"/>" + Tgt(5) +
                       "<p:attrNameLst><p:attrName>r</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>";
  Timing t = ParseXml(TimingXml(
      Group(Sub(0, Effect("entr", 3, "clickEffect", 0, Set(4, "visible") + Filter(4, "in", "blinds(horizontal)", 500)))) +
      Group(Sub(0, Effect("emph", 32, "clickEffect", 0, teeter))) +
      Group(Sub(0, Effect("entr", 21, "clickEffect", 0, Set(6, "visible") + Filter(6, "in", "wheel(1)", 500))))));
  CHECK_EQ(t.unsupportedEffects, 3);
  CHECK_EQ(t.initiallyHidden.size(), 2u);
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  a.Click(0);
  a.Update(0.5);
  CHECK(a.PoseOf(Shape(4)).Identity());
  CHECK_NEAR(a.Poses().size(), 2u, 0);
  a.Click(1000);
  CHECK(a.PoseOf(Shape(5)).Identity());
  CHECK(!a.Running(1000));  // nothing to play
  a.Click(2000);
  a.Update(2001);
  CHECK(a.PoseOf(Shape(6)).Identity());
}

TEST(SlideShowMotionPath) {
  std::string path = std::string("<p:animMotion origin=\"layout\" path=\"M 0 0 L 0.25 0 L 0.25 0.25 E\" pathEditMode=\"relative\">") +
                     "<p:cBhvr><p:cTn id=\"9\" dur=\"2000\" fill=\"hold\"/>" + Tgt(4) +
                     "<p:attrNameLst><p:attrName>ppt_x</p:attrName><p:attrName>ppt_y</p:attrName></p:attrNameLst>"
                     "</p:cBhvr></p:animMotion>";
  std::string curve = std::string("<p:animMotion path=\"M 0 0 C 0 0.1 0.1 0.1 0.1 0 E\">") +
                      "<p:cBhvr><p:cTn id=\"9\" dur=\"1000\" fill=\"hold\"/>" + Tgt(5) + "</p:cBhvr></p:animMotion>";
  Timing t = ParseXml(TimingXml(Group(Sub(0, Effect("path", 0, "clickEffect", 0, path))) +
                                Group(Sub(0, Effect("path", 1, "clickEffect", 0, curve)))));
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  a.Click(0);
  a.Update(500);
  CHECK_NEAR(a.PoseOf(Shape(4)).dx, 0.125, 1e-5);
  CHECK_NEAR(a.PoseOf(Shape(4)).dy, 0, 1e-5);
  a.Update(1500);
  CHECK_NEAR(a.PoseOf(Shape(4)).dx, 0.25, 1e-5);
  CHECK_NEAR(a.PoseOf(Shape(4)).dy, 0.125, 1e-5);
  a.Update(2000);
  CHECK_NEAR(a.PoseOf(Shape(4)).dy, 0.25, 1e-5);  // where the path ends, it stays
  a.Click(3000);
  a.Update(4000);
  CHECK_NEAR(a.PoseOf(Shape(5)).dx, 0.1, 1e-4);
  CHECK_NEAR(a.PoseOf(Shape(5)).dy, 0, 1e-4);
}

TEST(SlideShowTriggers) {
  // Clicking shape 7 fades shape 4 in; the main sequence is empty.
  std::string interactive =
      "<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=\"10\" restart=\"whenNotActive\" fill=\"hold\" "
      "evtFilter=\"cancelBubble\" nodeType=\"interactiveSeq\"><p:stCondLst><p:cond evt=\"onClick\" delay=\"0\">" +
      Tgt(7) + "</p:cond></p:stCondLst><p:endSync evt=\"end\" delay=\"0\"><p:rtn val=\"all\"/></p:endSync><p:childTnLst>"
      "<p:par><p:cTn id=\"11\" fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>" +
      Sub(0, Effect("entr", 10, "clickEffect", 0, FadeIn(4))) +
      "</p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn><p:nextCondLst><p:cond evt=\"onNext\" delay=\"0\">" +
      Tgt(7) + "</p:cond></p:nextCondLst></p:seq>";
  Timing t = ParseXml(TimingXml("", interactive));
  CHECK(t.main.steps.empty());
  CHECK_EQ(t.triggers.size(), 1u);
  CHECK_EQ(t.triggers[0].triggerSpid, 7u);
  CHECK_EQ(t.initiallyHidden.size(), 1u);
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  CHECK(!a.PoseOf(Shape(4)).visible);
  CHECK(!a.Click(0));
  CHECK(!a.Trigger(8, 0));
  CHECK(a.Trigger(7, 0));
  a.Update(250);
  CHECK_NEAR(a.PoseOf(Shape(4)).opacity, 0.5, 1e-6);
  a.Update(600);
  CHECK(a.PoseOf(Shape(4)).Identity());
  CHECK(!a.Trigger(7, 700));
}

TEST(SlideShowRepeatAndReverse) {
  // Spin with auto-reverse and two repeats on the effect: back at 0 at the end.
  std::string spin = "<p:animRot by=\"5400000\"><p:cBhvr><p:cTn id=\"9\" dur=\"1000\" fill=\"hold\"/>" + Tgt(4) +
                     "<p:attrNameLst><p:attrName>r</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>";
  Timing t = ParseXml(TimingXml(Group(Sub(0, Effect("emph", 8, "clickEffect", 0, spin, " repeatCount=\"2000\" autoRev=\"1\"")))));
  CHECK_NEAR(t.main.steps[0].length, 4000, 0.01);
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  a.Click(0);
  a.Update(500);
  CHECK_NEAR(a.PoseOf(Shape(4)).rotation, 45, 1e-3);
  a.Update(1500);
  CHECK_NEAR(a.PoseOf(Shape(4)).rotation, 45, 1e-3);
  a.Update(2500);
  CHECK_NEAR(a.PoseOf(Shape(4)).rotation, 45, 1e-3);
  a.Update(4000);
  CHECK_NEAR(a.PoseOf(Shape(4)).rotation, 0, 1e-3);
}

TEST(SlideShowTimingRobust) {
  // Not a timing, empty, and nonsense: nothing, and no crash.
  Timing t = ParseXml("<p:transition/>");
  CHECK(t.main.steps.empty());
  t = ParseXml("<p:timing/>");
  CHECK(t.main.steps.empty());
  t = ParseXml(TimingXml(Group(Sub(-5, Effect("entr", 10, "clickEffect", -3,
      "<p:anim><p:cBhvr><p:cTn dur=\"x\"/>" + Tgt(4) + "</p:cBhvr><p:tavLst><p:tav tm=\"abc\"/></p:tavLst></p:anim>"
      "<p:set><p:cBhvr><p:cTn/><p:tgtEl><p:sldTgt/></p:tgtEl></p:cBhvr></p:set>"
      "<p:animMotion path=\"M 1e400 0 L Q\"><p:cBhvr><p:cTn dur=\"100\"/>" + Tgt(4) + "</p:cBhvr></p:animMotion>")))));
  Animator a(&t, BoxOf, nullptr);
  a.Start(0);
  a.Click(0);
  a.Update(1e9);
  CHECK(!a.Running(1e9));
  // Deep nesting is cut off, not followed down.
  std::string deep;
  for (int i = 0; i < 100; ++i) deep += "<p:par><p:cTn><p:childTnLst>";
  deep += Set(4, "visible");
  for (int i = 0; i < 100; ++i) deep += "</p:childTnLst></p:cTn></p:par>";
  t = ParseXml(TimingXml(Group(deep)));
  CHECK_EQ(t.main.steps.size(), 1u);
  CHECK(t.main.steps[0].behaviors.empty());
  // Angles and points past a float's range: held, and every pose stays finite (fuzz_timing
  // crash-7d4b40c3, an animRot by 2.3e129).
  std::string spin = "<p:animRot by=\"2" + std::string(130, '3') + "\"><p:cBhvr><p:cTn id=\"9\" dur=\"1000\" fill=\"hold\"/>" +
                     Tgt(4) + "<p:attrNameLst><p:attrName>r</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>";
  std::string grow = "<p:animScale><p:cBhvr><p:cTn id=\"9\" dur=\"1000\" fill=\"hold\"/>" + Tgt(5) +
                     "</p:cBhvr><p:by x=\"1e300\" y=\"-1e300\"/></p:animScale>";
  std::string fly = "<p:animMotion><p:cBhvr><p:cTn id=\"9\" dur=\"1000\" fill=\"hold\"/>" + Tgt(6) +
                    "</p:cBhvr><p:by x=\"1e300\" y=\"-1e300\"/></p:animMotion>";
  t = ParseXml(TimingXml(Group(Sub(0, Effect("emph", 8, "clickEffect", 0, spin + grow + fly, " repeatCount=\"2000\" autoRev=\"1\"")))));
  CHECK_EQ(t.main.steps.size(), 1u);
  Animator big(&t, BoxOf, nullptr);
  big.Start(0);
  big.Click(0);
  for (double ms : {250.0, 1000.0, 2500.0, 5000.0}) {
    big.Update(ms);
    CHECK_EQ(big.Poses().size(), 3u);
    for (const auto& [key, p] : big.Poses())
      CHECK(std::isfinite(p.rotation) && std::isfinite(p.sx) && std::isfinite(p.sy) && std::isfinite(p.dx) &&
            std::isfinite(p.dy));
  }
  // An animator without a timing does nothing.
  Animator none;
  none.Start(0);
  CHECK(!none.Click(0));
  CHECK(none.PoseOf(Shape(4)).Identity());
}

// ---- The decks PowerPoint made (tests/office/show/make_show_decks.ps1) ----

namespace {

struct Deck {
  std::unique_ptr<fw::pkg::Package> pkg;
  explicit Deck(const char* name) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "show" / name;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    fw::pkg::Status st;
    pkg = test::OpenBytes(std::move(bytes), st);
    CHECK(pkg != nullptr);
  }
  // Slide n's DOM (1-based).
  bool Slide(int n, fw::pkg::XmlDom& dom) const {
    std::vector<uint8_t> data;
    if (!pkg || !pkg->ReadPart("/ppt/slides/slide" + std::to_string(n) + ".xml", data).Ok()) return false;
    return dom.Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size())).Ok();
  }
};

pugi::xml_node Child(pugi::xml_node root, const char* local) {
  for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling())
    if (fw::pkg::LocalNameOf(c) == local) return c;
  return {};
}

Timing DeckTiming(const Deck& d, int n) {
  fw::pkg::XmlDom dom;
  CHECK(d.Slide(n, dom));
  return ParseTiming(Child(dom.Root(), "timing"));
}

Transition DeckTransition(const Deck& d, int n) {
  fw::pkg::XmlDom dom;
  CHECK(d.Slide(n, dom));
  pugi::xml_node t = Child(dom.Root(), "transition");
  return ParseTransition(t ? t : Child(dom.Root(), "AlternateContent"));
}

}  // namespace

TEST(SlideShowPowerPointTransitions) {
  Deck d("transitions.pptx");
  Transition t = DeckTransition(d, 1);
  CHECK(t.kind == TransitionKind::Fade && !t.throughBlack);
  CHECK_NEAR(t.durationMs, 1000, 0.01);
  CHECK(DeckTransition(d, 2).throughBlack);
  t = DeckTransition(d, 4);
  CHECK(t.kind == TransitionKind::Push && t.side == Side::Down);
  CHECK(DeckTransition(d, 5).side == Side::Left);
  t = DeckTransition(d, 11);
  CHECK(t.kind == TransitionKind::Wipe && t.side == Side::Down);
  t = DeckTransition(d, 15);
  CHECK(t.kind == TransitionKind::Split && t.vertical && t.in);
  t = DeckTransition(d, 20);
  CHECK(t.kind == TransitionKind::Cover && t.side == Side::RightDown);
  t = DeckTransition(d, 25);
  CHECK(t.kind == TransitionKind::Uncover && t.side == Side::LeftUp);
  t = DeckTransition(d, 26);
  CHECK(t.kind == TransitionKind::Zoom && t.in);
  t = DeckTransition(d, 28);  // vortex
  CHECK(t.kind == TransitionKind::Fade && !t.played);
  t = DeckTransition(d, 32);  // cut
  CHECK(t.kind == TransitionKind::None);
  t = DeckTransition(d, 33);
  CHECK(t.kind == TransitionKind::Push && t.side == Side::Up);
  CHECK_NEAR(t.durationMs, 2000, 0.01);
  CHECK(!t.advanceOnClick);
  CHECK_NEAR(t.advanceAfterMs, 3000, 0.01);
}

TEST(SlideShowPowerPointAnimations) {
  Deck d("animations.pptx");
  Timing t = DeckTiming(d, 1);  // appear, fade, fly in from the bottom
  CHECK_EQ(t.main.steps.size(), 3u);
  CHECK_EQ(t.initiallyHidden.size(), 3u);
  CHECK_EQ(t.unsupportedEffects, 0);
  t = DeckTiming(d, 2);  // four flies on one click
  CHECK_EQ(t.main.steps.size(), 1u);
  CHECK_NEAR(t.main.steps[0].length, 750, 0.01);
  t = DeckTiming(d, 3);  // wipe from bottom, wipe from left, split vertical out, zoom
  CHECK_EQ(t.main.steps.size(), 4u);
  CHECK_EQ(t.unsupportedEffects, 0);
  bool wipe = false, split = false;
  for (const Behavior& b : t.main.steps[0].behaviors) wipe |= b.reveal == Reveal::Wipe && b.side == Side::Down;
  for (const Behavior& b : t.main.steps[2].behaviors) split |= b.reveal == Reveal::Split && b.vertical && b.outward;
  CHECK(wipe);
  CHECK(split);
  t = DeckTiming(d, 4);  // float in, faded zoom, grow and turn (appear), float out
  CHECK_EQ(t.unsupportedEffects, 1);
  CHECK_EQ(t.main.steps.size(), 4u);
  t = DeckTiming(d, 5);  // grow, spin, teeter (nothing)
  CHECK_EQ(t.unsupportedEffects, 1);
  CHECK(t.initiallyHidden.empty());
  CHECK(t.main.steps[2].behaviors.empty());
  t = DeckTiming(d, 7);  // by paragraph
  CHECK_EQ(t.main.steps.size(), 4u);
  CHECK_EQ(t.byParagraph.size(), 1u);
  CHECK_EQ(t.initiallyHidden.size(), 4u);
  t = DeckTiming(d, 8);  // A on click; B with it after 0.5 s; C after B; D after C after 1 s
  CHECK_EQ(t.main.steps.size(), 1u);
  CHECK_NEAR(t.main.steps[0].length, 3500, 0.01);
  t = DeckTiming(d, 9);  // starts by itself after 1 s, then a click
  CHECK_EQ(t.main.steps.size(), 2u);
  CHECK(t.main.steps[0].start != StepStart::Click);
  CHECK(t.main.steps[1].start == StepStart::Click);
  t = DeckTiming(d, 10);  // the trigger
  CHECK(t.main.steps.empty());
  CHECK_EQ(t.triggers.size(), 1u);
  CHECK_EQ(t.triggers[0].triggerSpid, 3u);
  CHECK_EQ(t.initiallyHidden.size(), 1u);
  t = DeckTiming(d, 11);  // motion paths, with the effect's smooth start and end
  CHECK_EQ(t.main.steps.size(), 3u);
  for (const Step& s : t.main.steps) {
    CHECK_EQ(s.behaviors.size(), 1u);
    CHECK(s.behaviors[0].kind == Behavior::Motion && s.behaviors[0].path.size() >= 2);
    CHECK_NEAR(s.behaviors[0].accel, 0.5, 1e-9);
  }
  t = DeckTiming(d, 12);  // repeat twice, auto-reverse
  CHECK_NEAR(t.main.steps[0].length, 4000, 0.01);
  t = DeckTiming(d, 13);  // wheel, blinds, checkerboard: appear
  CHECK_EQ(t.unsupportedEffects, 3);

  // Played through, every slide ends where PowerPoint ends: entrances shown, exits hidden.
  Animator a;
  t = DeckTiming(d, 6);
  a = Animator(&t, BoxOf, nullptr);
  a.FinishAll(0);
  for (const auto& [key, pose] : a.Poses()) CHECK(!pose.visible);
  t = DeckTiming(d, 3);
  a = Animator(&t, BoxOf, nullptr);
  a.FinishAll(0);
  for (const auto& [key, pose] : a.Poses()) CHECK(pose.Identity());
}

TEST(SlideShowOrder) {
  // Seven slides; 2 and 5 hidden.
  auto hidden = [](size_t i) { return i == 2 || i == 5; };
  const size_t E = SlideOrder::kEnd;
  SlideOrder o(7, hidden, false);
  CHECK(o.First() == 0);
  CHECK(o.Next(1) == 3);
  CHECK(o.Next(4) == 6);
  CHECK(o.Next(6) == E);
  CHECK(o.Prev(3) == 1);
  CHECK(o.Prev(E) == 6);
  CHECK(o.Prev(0) == E);
  SlideOrder none(3, [](size_t) { return true; }, false);
  CHECK(none.First() == E);
  SlideOrder loop(7, hidden, true);
  CHECK(loop.Next(6) == 0);

  // A custom show (1, 4, 2: a hidden slide plays in it) started from slide 3, coming back there.
  CHECK(!o.StartCustom({1, 9}, 3, false));  // a slide the deck hasn't
  CHECK(!o.InCustom());
  CHECK(o.StartCustom({1, 4, 2}, 3, false));
  CHECK(o.First() == 1);
  o.Moved(1);
  size_t s = o.Advance(1);
  CHECK(s == 4);
  o.Moved(s);
  s = o.Advance(4);
  CHECK(s == 2);
  o.Moved(s);
  CHECK(o.Prev(2) == 4);
  s = o.Advance(2);
  CHECK(s == 3);
  o.Moved(s);
  CHECK(!o.InCustom());
  CHECK(o.Next(3) == 4);  // on through the deck

  // Started from slide 1, which is in it: back to slide 1, and the deck goes on from there.
  CHECK(o.StartCustom({1, 4}, 1, false));
  o.Moved(1);
  s = o.Advance(1);
  CHECK(s == 4);
  o.Moved(s);
  s = o.Advance(4);
  CHECK(s == 1);
  o.Moved(s);
  CHECK(!o.InCustom());
  CHECK(o.Next(1) == 3);

  // Not coming back: the end of the show after its last slide.
  CHECK(o.StartCustom({4, 6}, E, false));
  o.Moved(4);
  s = o.Advance(4);
  CHECK(s == 6);
  o.Moved(s);
  CHECK(o.Advance(6) == E);
  CHECK(!o.InCustom());

  // Back before its first slide: to the slide that started it, and out of it.
  CHECK(o.StartCustom({4, 6}, 0, false));
  o.Moved(4);
  s = o.Back(4);
  CHECK(s == 0);
  o.Moved(s);
  CHECK(!o.InCustom());
  // With no slide to go back to, back does nothing and it goes on.
  CHECK(o.StartCustom({4, 6}, E, false));
  o.Moved(4);
  CHECK(o.Back(4) == E);
  CHECK(o.InCustom());
  // A jump to one of its slides keeps the place; to another slide, leaves it.
  o.Moved(6);
  CHECK(o.InCustom());
  CHECK(o.Prev(6) == 4);
  o.Moved(0);
  CHECK(!o.InCustom());

  // The deck's own custom show, looping.
  SlideOrder own(7, hidden, true);
  CHECK(own.StartCustom({3, 1}, E, true));
  CHECK(own.First() == 3);
  own.Moved(3);
  s = own.Advance(3);
  CHECK(s == 1);
  own.Moved(s);
  s = own.Advance(1);
  CHECK(s == 3);
  own.Moved(s);
  CHECK(own.InCustom());
  CHECK(own.Next(3) == 1);
}

TEST(SlideShowOrderOnDecks) {
  // p1-corpus's public decks (tests/office/presentations), read by the slide model.
  auto open = [](const char* name) {
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "presentations" / name;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    fw::pkg::Status st;
    auto pkg = test::OpenBytes(std::move(bytes), st);
    return pkg ? fw::slide::Read(std::move(pkg), st) : nullptr;
  };
  // The button on slide 1 plays the custom show (slides 1, 4, 6) and comes back.
  auto deck = open("sections-custom-shows.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  const fw::slide::Slide* first = deck->GetSlide(0);
  const fw::slide::Hyperlink* link = nullptr;
  for (const fw::slide::Shape& sh : first->shapes)
    if (sh.click.action == fw::slide::Hyperlink::Action::CustomShow) link = &sh.click;
  CHECK(link != nullptr);
  if (!link) return;
  CHECK(link->returns);
  const fw::slide::CustomShow* cs = nullptr;
  for (const fw::slide::CustomShow& c : deck->CustomShows())
    if (c.id == link->customShow) cs = &c;
  CHECK(cs != nullptr);
  if (!cs) return;
  fw::slide::Presentation* d = deck.get();
  SlideOrder o(deck->SlideCount(), [d](size_t i) { return d->GetSlide(i)->hidden; }, false);
  CHECK(o.StartCustom(cs->slides, 0, false));
  std::vector<size_t> played;
  for (size_t s = o.First(); played.size() < 10; s = o.Advance(s)) {
    o.Moved(s);
    played.push_back(s);
    if (!o.InCustom()) break;
  }
  CHECK((played == std::vector<size_t>{0, 3, 5, 0}));

  // hyperlinks.pptx: the oval's mouse-over link goes to slide 3.
  deck = open("hyperlinks.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  const fw::slide::Shape* oval = nullptr;
  for (const fw::slide::Shape& sh : deck->GetSlide(0)->shapes)
    if (sh.id == 10) oval = &sh;
  CHECK(oval != nullptr);
  if (!oval) return;
  CHECK(oval->hover.action == fw::slide::Hyperlink::Action::Slide);
  CHECK(oval->hover.slide == 2);
}
