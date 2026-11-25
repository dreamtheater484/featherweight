// Transitions and animations (src/slide/edit/TimingOps, P3 p3-anim): written as PowerPoint writes
// them, played by the show's Timing reader, undone and redone, and what Featherweight can't change
// kept byte for byte.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Save.h"
#include "slide/edit/TimingOps.h"
#include "slide/show/Timing.h"

using namespace fw::slide;
using namespace fw::slide::edit;
namespace show = fw::slide::show;

namespace {

std::filesystem::path Generated() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "generated";
}

std::vector<uint8_t> FileBytes(const std::filesystem::path& p) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(p.wstring(), {}, st);
  if (!snap) return {};
  auto span = snap->Bytes();
  return std::vector<uint8_t>(span.begin(), span.end());
}

std::unique_ptr<Presentation> OpenBytes(std::vector<uint8_t> bytes) {
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), fw::pkg::Limits::Default(), st);
  if (!pk) return nullptr;
  return Read(std::move(pk), st);
}

std::unique_ptr<Presentation> Open(const char* name) { return OpenBytes(FileBytes(Generated() / name)); }

std::string SlideXml(const Editor& ed, size_t slide) {
  std::vector<uint8_t> b;
  ed.ReadPart(ed.SlidePart(slide), b);
  return std::string(b.begin(), b.end());
}

size_t Count(const std::string& s, const std::string& sub) {
  size_t n = 0;
  for (size_t at = s.find(sub); at != std::string::npos; at = s.find(sub, at + sub.size())) ++n;
  return n;
}

bool Has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// The slide's XML replaced (a deck made by another app), as one step.
bool WriteSlide(Editor& ed, size_t slide, const std::string& xml) {
  std::string part = ed.SlidePart(slide);
  return static_cast<bool>(ed.Run(u"Write", [&] {
    return ed.WritePart(part, std::vector<uint8_t>(xml.begin(), xml.end())) ? Result{} : Result::Fail(u"write");
  }));
}

// Every cTn id in the slide once.
bool IdsUnique(const std::string& xml) {
  std::vector<std::string> ids;
  for (size_t at = xml.find("<p:cTn id=\""); at != std::string::npos; at = xml.find("<p:cTn id=\"", at + 1)) {
    size_t s = at + 11;
    ids.push_back(xml.substr(s, xml.find('"', s) - s));
  }
  std::sort(ids.begin(), ids.end());
  return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

const char* kHead =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><p:cSld><p:spTree>"
    "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
    "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
    "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>Title</a:t></a:r></a:p></p:txBody></p:sp>"
    "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Body\"/><p:cNvSpPr/><p:nvPr><p:ph idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/>"
    "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>One</a:t></a:r></a:p><a:p><a:pPr lvl=\"1\"/><a:r><a:t>One.a</a:t></a:r></a:p>"
    "<a:p><a:endParaRPr/></a:p><a:p><a:r><a:t>Two</a:t></a:r></a:p></p:txBody></p:sp>"
    "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>";

// A Bounce entrance (presetID 26) Featherweight can't change, as PowerPoint writes it.
const char* kBounce =
    "<p:par><p:cTn id=\"5\" presetID=\"26\" presetClass=\"entr\" presetSubtype=\"0\" fill=\"hold\" grpId=\"0\" nodeType=\"clickEffect\">"
    "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>"
    "<p:set><p:cBhvr><p:cTn id=\"6\" dur=\"1\" fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst></p:cTn>"
    "<p:tgtEl><p:spTgt spid=\"2\"/></p:tgtEl><p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst></p:cBhvr>"
    "<p:to><p:strVal val=\"visible\"/></p:to></p:set>"
    "<p:animEffect transition=\"in\" filter=\"wipe(down)\"><p:cBhvr><p:cTn id=\"7\" dur=\"580\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst></p:cTn>"
    "<p:tgtEl><p:spTgt spid=\"2\"/></p:tgtEl></p:cBhvr></p:animEffect>"
    "<p:anim calcmode=\"lin\" valueType=\"num\"><p:cBhvr><p:cTn id=\"8\" dur=\"1822\" tmFilter=\"0,0; 0.14,0.36; 0.43,0.73; 0.71,0.91; 1.0,1.0\">"
    "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst></p:cTn><p:tgtEl><p:spTgt spid=\"2\"/></p:tgtEl>"
    "<p:attrNameLst><p:attrName>ppt_x</p:attrName></p:attrNameLst></p:cBhvr><p:tavLst><p:tav tm=\"0\"><p:val><p:strVal val=\"#ppt_x-0.25\"/></p:val></p:tav>"
    "<p:tav tm=\"100000\"><p:val><p:strVal val=\"#ppt_x\"/></p:val></p:tav></p:tavLst></p:anim>"
    "</p:childTnLst></p:cTn></p:par>";

std::string TimingWith(const std::string& effects, const std::string& builds) {
  return std::string(
             "<p:timing><p:tnLst><p:par><p:cTn id=\"1\" dur=\"indefinite\" restart=\"never\" nodeType=\"tmRoot\"><p:childTnLst>"
             "<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=\"2\" dur=\"indefinite\" nodeType=\"mainSeq\"><p:childTnLst>"
             "<p:par><p:cTn id=\"3\" fill=\"hold\"><p:stCondLst><p:cond delay=\"indefinite\"/></p:stCondLst><p:childTnLst>"
             "<p:par><p:cTn id=\"4\" fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>") +
         effects +
         "</p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn>"
         "<p:prevCondLst><p:cond evt=\"onPrev\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst>"
         "<p:nextCondLst><p:cond evt=\"onNext\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:nextCondLst></p:seq>"
         "</p:childTnLst></p:cTn></p:par></p:tnLst>" +
         builds + "</p:timing>";
}

}  // namespace

TEST(SlideAnimTransitionsAsPowerPointWritesThem) {
  auto deck = Open("shapes-table-chart.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  CHECK(GetTransition(ed, 0).effect == TransitionEffect::None);

  // Push at its own duration (1 s, spd slow): a plain p:transition.
  TransitionPatch push;
  push.effect = TransitionEffect::Push;
  CHECK(SetTransition(ed, {0}, push));
  std::string x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:transition spd=\"slow\"><p:push dir=\"u\"/></p:transition><"));
  CHECK(!Has(x, "AlternateContent"));
  CHECK(x.find("</p:clrMapOvr><p:transition") != std::string::npos);
  TransitionInfo t = GetTransition(ed, 0);
  CHECK(t.effect == TransitionEffect::Push && t.option == 0 && t.durationMs == 1000);
  show::Transition played = show::ParseTransition(deck->GetSlide(0)->transition);
  CHECK(played.kind == show::TransitionKind::Push && played.side == show::Side::Up && played.durationMs == 1000);

  // Another duration: a p14 Choice with p14:dur and the plain Fallback.
  TransitionPatch longer;
  longer.durationMs = 2000;
  CHECK(SetTransition(ed, {0}, longer));
  x = SlideXml(ed, 0);
  CHECK(Has(x, "<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\"><mc:Choice "
               "xmlns:p14=\"http://schemas.microsoft.com/office/powerpoint/2010/main\" Requires=\"p14\"><p:transition "
               "spd=\"slow\" p14:dur=\"2000\"><p:push dir=\"u\"/></p:transition></mc:Choice><mc:Fallback><p:transition "
               "spd=\"slow\"><p:push dir=\"u\"/></p:transition></mc:Fallback></mc:AlternateContent>"));
  CHECK(GetTransition(ed, 0).durationMs == 2000);
  CHECK(show::ParseTransition(deck->GetSlide(0)->transition).durationMs == 2000);

  // Fade at its default 0.7 s, through black; Split's options; advancing.
  TransitionPatch fade;
  fade.effect = TransitionEffect::Fade;
  fade.option = 1;
  fade.advanceOnClick = false;
  fade.advanceAfterMs = 3000;
  CHECK(SetTransition(ed, {0, 1}, fade));
  x = SlideXml(ed, 1);
  CHECK(Has(x, "<p:transition spd=\"med\" p14:dur=\"700\" advClick=\"0\" advTm=\"3000\"><p:fade thruBlk=\"1\"/></p:transition>"));
  CHECK(Has(x, "<mc:Fallback><p:transition spd=\"med\" advClick=\"0\" advTm=\"3000\"><p:fade thruBlk=\"1\"/>"));
  t = GetTransition(ed, 1);
  CHECK(t.effect == TransitionEffect::Fade && t.option == 1 && t.durationMs == 700 && !t.advanceOnClick && t.advanceAfterMs == 3000);
  played = show::ParseTransition(deck->GetSlide(1)->transition);
  CHECK(played.kind == show::TransitionKind::Fade && played.throughBlack && !played.advanceOnClick && played.advanceAfterMs == 3000);

  TransitionPatch split;
  split.effect = TransitionEffect::Split;
  split.option = 1;  // Vertical In
  CHECK(SetTransition(ed, {0}, split));
  x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:split orient=\"vert\" dir=\"in\"/>"));
  CHECK(Has(x, "p14:dur=\"1500\""));
  CHECK(GetTransition(ed, 0).option == 1);
  CHECK(GetTransition(ed, 0).advanceAfterMs == 3000);  // kept from before

  // None with advancing kept; None without: no transition at all.
  TransitionPatch none;
  none.effect = TransitionEffect::None;
  CHECK(SetTransition(ed, {0}, none));
  x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:transition advClick=\"0\" advTm=\"3000\"/>") || Has(x, "<p:transition advClick=\"0\" advTm=\"3000\"></p:transition>"));
  TransitionPatch clear;
  clear.advanceOnClick = true;
  clear.advanceAfterMs = -1;
  CHECK(SetTransition(ed, {0}, clear));
  CHECK(!Has(SlideXml(ed, 0), "transition"));

  // Out of range; Other isn't a choice.
  TransitionPatch bad;
  bad.durationMs = 60000;
  CHECK(!SetTransition(ed, {0}, bad));
  bad = {};
  bad.effect = TransitionEffect::Other;
  CHECK(!SetTransition(ed, {0}, bad));
  bad = {};
  bad.effect = TransitionEffect::Push;
  bad.option = 4;
  CHECK(!SetTransition(ed, {0}, bad));

  // Undo all the way; saved and opened again.
  while (ed.CanUndo()) CHECK(ed.Undo());
  CHECK(GetTransition(ed, 0).effect == TransitionEffect::None);
  CHECK(!Has(SlideXml(ed, 1), "transition"));
  while (ed.CanRedo()) CHECK(ed.Redo());
  CHECK(GetTransition(ed, 1).effect == TransitionEffect::Fade);
  std::vector<uint8_t> saved;
  CHECK(SaveToBytes(ed, saved).Ok());
  auto again = OpenBytes(saved);
  CHECK(again != nullptr);
  Editor ed2(*again);
  t = GetTransition(ed2, 1);
  CHECK(t.effect == TransitionEffect::Fade && t.option == 1 && t.durationMs == 700 && t.advanceAfterMs == 3000);
}

TEST(SlideAnimOtherTransitionsKeepTheirEffect) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string airplane =
      "<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\"><mc:Choice "
      "xmlns:p15=\"http://schemas.microsoft.com/office/powerpoint/2012/main\" Requires=\"p15\"><p:transition spd=\"slow\" "
      "xmlns:p14=\"http://schemas.microsoft.com/office/powerpoint/2010/main\" p14:dur=\"1250\"><p15:prstTrans prst=\"airplane\"/>"
      "</p:transition></mc:Choice><mc:Fallback><p:transition spd=\"slow\"><p:fade/></p:transition></mc:Fallback></mc:AlternateContent>";
  CHECK(WriteSlide(ed, 0, std::string(kHead) + airplane + "</p:sld>"));
  TransitionInfo t = GetTransition(ed, 0);
  CHECK(t.effect == TransitionEffect::Other && t.durationMs == 1250);

  TransitionPatch p;
  p.durationMs = 3000;
  p.advanceAfterMs = 5000;
  CHECK(SetTransition(ed, {0}, p));
  std::string x = SlideXml(ed, 0);
  CHECK(Has(x, "<p15:prstTrans prst=\"airplane\"/>"));
  CHECK(Has(x, "p14:dur=\"3000\""));
  CHECK(Count(x, "advTm=\"5000\"") == 2);
  CHECK(Has(x, "<mc:Fallback><p:transition spd=\"slow\" advTm=\"5000\"><p:fade/>"));
  t = GetTransition(ed, 0);
  CHECK(t.effect == TransitionEffect::Other && t.durationMs == 3000 && t.advanceAfterMs == 5000);
  TransitionPatch opt;
  opt.option = 1;
  CHECK(!SetTransition(ed, {0}, opt));

  // A plain one of the old effects (Dissolve) given a duration of its own: wrapped as PowerPoint does.
  CHECK(WriteSlide(ed, 1, std::string(kHead) + "<p:transition spd=\"med\"><p:dissolve/></p:transition></p:sld>"));
  CHECK(GetTransition(ed, 1).effect == TransitionEffect::Other);
  TransitionPatch d;
  d.durationMs = 1200;
  CHECK(SetTransition(ed, {1}, d));
  x = SlideXml(ed, 1);
  CHECK(Has(x, "Requires=\"p14\"><p:transition spd=\"slow\" p14:dur=\"1200\"><p:dissolve/></p:transition></mc:Choice><mc:Fallback><p:transition spd=\"slow\"><p:dissolve/></p:transition></mc:Fallback>"));
  CHECK(GetTransition(ed, 1).durationMs == 1200);

  // Picking a common one replaces it.
  TransitionPatch wipe;
  wipe.effect = TransitionEffect::Wipe;
  wipe.option = 3;
  CHECK(SetTransition(ed, {0}, wipe));
  x = SlideXml(ed, 0);
  CHECK(!Has(x, "prstTrans"));
  CHECK(Has(x, "<p:transition spd=\"slow\" advTm=\"5000\"><p:wipe dir=\"u\"/></p:transition>"));
}

TEST(SlideAnimApplyTransitionToAll) {
  auto deck = Open("layouts.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  TransitionPatch p;
  p.effect = TransitionEffect::Cover;
  p.option = 5;  // From Bottom-Right
  p.durationMs = 800;
  CHECK(SetTransition(ed, {2}, p));
  CHECK(ApplyTransitionToAll(ed, 2));
  CHECK(ed.UndoName() == u"Apply To All");
  for (size_t i = 0; i < deck->SlideCount(); ++i) {
    TransitionInfo t = GetTransition(ed, i);
    CHECK(t.effect == TransitionEffect::Cover && t.option == 5 && t.durationMs == 800);
    CHECK(Count(SlideXml(ed, i), "<p:cover dir=\"lu\"/>") == 2);
  }
  CHECK(ed.Undo());
  CHECK(GetTransition(ed, 0).effect == TransitionEffect::None);
  CHECK(GetTransition(ed, 2).effect == TransitionEffect::Cover);
}

TEST(SlideAnimAnimationsAsPowerPointWritesThem) {
  auto deck = Open("shapes-table-chart.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::u16string why;
  CHECK(MainSequence(ed, 0, &why).empty() && why.empty());

  AnimSpec fade;
  fade.anim = Anim::Fade;
  CHECK(AddAnimation(ed, 0, 2, fade));
  AnimSpec fly;
  fly.anim = Anim::FlyIn;
  fly.trigger = AnimTrigger::AfterPrevious;
  CHECK(AddAnimation(ed, 0, 5, fly));  // the picture
  AnimSpec wipe;
  wipe.anim = Anim::Wipe;
  wipe.trigger = AnimTrigger::WithPrevious;
  CHECK(AddAnimation(ed, 0, 3, wipe));  // the star: no text
  CHECK(ed.UndoName() == u"Add Animation");

  std::vector<AnimEffect> seq = MainSequence(ed, 0, &why);
  CHECK(why.empty());
  CHECK_EQ(seq.size(), 3u);
  CHECK(seq[0].shape == 2 && seq[0].anim == Anim::Fade && seq[0].trigger == AnimTrigger::OnClick && seq[0].durationMs == 500);
  CHECK(seq[1].shape == 5 && seq[1].anim == Anim::FlyIn && seq[1].option == 0 && seq[1].trigger == AnimTrigger::AfterPrevious);
  CHECK(seq[2].shape == 3 && seq[2].anim == Anim::Wipe && seq[2].trigger == AnimTrigger::WithPrevious && seq[2].editable);

  std::string x = SlideXml(ed, 0);
  CHECK(IdsUnique(x));
  // One click group: the fade, then (after it, 500 ms in) the fly-in with the wipe.
  CHECK_EQ(Count(x, "<p:cond delay=\"indefinite\"/>"), 1u);
  CHECK(Has(x, "<p:cTn id=\"6\" fill=\"hold\"><p:stCondLst><p:cond delay=\"500\"/></p:stCondLst>") ||
        Has(x, "fill=\"hold\"><p:stCondLst><p:cond delay=\"500\"/></p:stCondLst><p:childTnLst><p:par><p:cTn"));
  CHECK(Has(x, "presetID=\"10\" presetClass=\"entr\" presetSubtype=\"0\" fill=\"hold\" grpId=\"0\" nodeType=\"clickEffect\""));
  CHECK(Has(x, "presetID=\"2\" presetClass=\"entr\" presetSubtype=\"4\" fill=\"hold\" nodeType=\"afterEffect\""));
  CHECK(Has(x, "presetID=\"22\" presetClass=\"entr\" presetSubtype=\"4\" fill=\"hold\" grpId=\"0\" nodeType=\"withEffect\""));
  CHECK(Has(x, "<p:strVal val=\"1+#ppt_h/2\"/>"));
  CHECK(Has(x, "filter=\"wipe(down)\""));
  // Builds: the text shape as a whole, the star with its background, the picture none.
  CHECK(Has(x, "<p:bldLst><p:bldP spid=\"2\" grpId=\"0\"/><p:bldP spid=\"3\" grpId=\"0\" animBg=\"1\"/></p:bldLst></p:timing>"));
  CHECK(x.find("</p:clrMapOvr><p:timing>") != std::string::npos);

  // The show plays it: one click, three effects.
  show::Timing timing = show::ParseTiming(deck->GetSlide(0)->timing);
  CHECK_EQ(timing.main.steps.size(), 1u);
  CHECK_EQ(timing.unsupportedEffects, 0);
  CHECK(timing.main.steps[0].length >= 999);

  // Timing: the fade 1 s long moves the group after it.
  AnimTimingPatch longer;
  longer.durationMs = 1000;
  CHECK(SetAnimationTiming(ed, 0, {0}, longer));
  x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:cond delay=\"1000\"/>"));
  CHECK(MainSequence(ed, 0)[0].durationMs == 1000);
  AnimTimingPatch delay;
  delay.delayMs = 250;
  CHECK(SetAnimationTiming(ed, 0, {1}, delay));
  CHECK(MainSequence(ed, 0)[1].delayMs == 250);
  // On Click: a second click group.
  AnimTimingPatch click;
  click.trigger = AnimTrigger::OnClick;
  CHECK(SetAnimationTiming(ed, 0, {1}, click));
  x = SlideXml(ed, 0);
  CHECK_EQ(Count(x, "<p:cond delay=\"indefinite\"/>"), 2u);
  CHECK(IdsUnique(x));
  CHECK_EQ(show::ParseTiming(deck->GetSlide(0)->timing).main.steps.size(), 2u);
  seq = MainSequence(ed, 0);
  CHECK(seq[1].trigger == AnimTrigger::OnClick && seq[2].trigger == AnimTrigger::WithPrevious);

  // Moved first: the wipe starts with the slide.
  CHECK(MoveAnimations(ed, 0, {2}, 0));
  seq = MainSequence(ed, 0);
  CHECK(seq[0].shape == 3 && seq[0].trigger == AnimTrigger::WithPrevious && seq[1].shape == 2);
  x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:cond delay=\"indefinite\"/><p:cond evt=\"onBegin\" delay=\"0\"><p:tn val=\"2\"/></p:cond>"));
  timing = show::ParseTiming(deck->GetSlide(0)->timing);
  CHECK(!timing.main.steps.empty() && timing.main.steps[0].start == show::StepStart::WithPrevious);

  // Change and options.
  CHECK(ChangeAnimation(ed, 0, {1}, Anim::FlyIn));
  CHECK(SetAnimationOption(ed, 0, {1}, 2));  // From Left
  seq = MainSequence(ed, 0);
  CHECK(seq[1].anim == Anim::FlyIn && seq[1].option == 2 && seq[1].durationMs == 500);
  CHECK(Has(SlideXml(ed, 0), "<p:strVal val=\"0-#ppt_w/2\"/>"));
  CHECK(!SetAnimationOption(ed, 0, {1}, 8));

  // Removing them all takes the timing.
  CHECK(RemoveAnimations(ed, 0, {0, 1, 2}));
  CHECK(!Has(SlideXml(ed, 0), "timing"));
  CHECK(ed.Undo());
  CHECK_EQ(MainSequence(ed, 0).size(), 3u);
  CHECK(RemoveAnimations(ed, 0, {1}));
  x = SlideXml(ed, 0);
  CHECK(!Has(x, "<p:bldP spid=\"2\""));
  CHECK(Has(x, "<p:bldP spid=\"3\""));

  // Saved and opened again: the same sequence.
  std::vector<uint8_t> saved;
  CHECK(SaveToBytes(ed, saved).Ok());
  auto again = OpenBytes(saved);
  CHECK(again != nullptr);
  Editor ed2(*again);
  CHECK_EQ(MainSequence(ed2, 0).size(), 2u);
  while (ed.CanUndo()) CHECK(ed.Undo());
  CHECK(!Has(SlideXml(ed, 0), "timing"));
}

TEST(SlideAnimEveryCommonAnimationPlays) {
  auto deck = Open("shapes-table-chart.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  const Anim all[] = {Anim::Appear,     Anim::Fade,      Anim::FlyIn,   Anim::FloatIn, Anim::Split,    Anim::Wipe,
                      Anim::Zoom,       Anim::GrowShrink, Anim::Spin,    Anim::Disappear, Anim::FadeOut, Anim::FlyOut,
                      Anim::FloatOut,   Anim::SplitOut,  Anim::WipeOut, Anim::ZoomOut};
  for (Anim a : all) {
    for (int o = 0; o < std::max<int>(1, static_cast<int>(AnimOptions(a).size())); ++o) {
      AnimSpec s;
      s.anim = a;
      s.option = o;
      CHECK(AddAnimation(ed, 0, 2, s));
    }
  }
  std::vector<AnimEffect> seq = MainSequence(ed, 0);
  size_t i = 0;
  for (Anim a : all) {
    for (int o = 0; o < std::max<int>(1, static_cast<int>(AnimOptions(a).size())); ++o, ++i) {
      CHECK(i < seq.size());
      if (i >= seq.size()) break;
      CHECK(seq[i].anim == a);
      CHECK(seq[i].option == o);
      CHECK(seq[i].editable);
      CHECK(seq[i].durationMs == DefaultAnimMs(a));
    }
  }
  std::string x = SlideXml(ed, 0);
  CHECK(IdsUnique(x));
  show::Timing timing = show::ParseTiming(deck->GetSlide(0)->timing);
  CHECK_EQ(timing.unsupportedEffects, 0);
  CHECK_EQ(timing.main.steps.size(), seq.size());
  // Each shape effect its own group: grpId 0, 1, 2...
  CHECK(Has(x, "<p:bldP spid=\"2\" grpId=\"" + std::to_string(seq.size() - 1) + "\"/>"));
  // Durations scale every behavior: the fade-out's last set stays 1 ms before its end.
  size_t fadeOut = 0;
  for (size_t k = 0; k < seq.size(); ++k)
    if (seq[k].anim == Anim::FadeOut) fadeOut = k;
  AnimTimingPatch p;
  p.durationMs = 2000;
  CHECK(SetAnimationTiming(ed, 0, {fadeOut}, p));
  CHECK(MainSequence(ed, 0)[fadeOut].durationMs == 2000);
  CHECK(Has(SlideXml(ed, 0), "<p:cond delay=\"1999\"/>"));
}

TEST(SlideAnimByParagraph) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  CHECK(WriteSlide(ed, 0, std::string(kHead) + "</p:sld>"));
  AnimSpec s;
  s.anim = Anim::Fade;
  s.byParagraph = true;
  s.trigger = AnimTrigger::WithPrevious;
  CHECK(AddAnimation(ed, 0, 3, s));
  std::vector<AnimEffect> seq = MainSequence(ed, 0);
  // "One" with its sub-point, then "Two" (the empty paragraph left out).
  CHECK_EQ(seq.size(), 2u);
  CHECK(seq[0].paragraph == 0 && seq[0].trigger == AnimTrigger::WithPrevious);
  CHECK(seq[1].paragraph == 3 && seq[1].trigger == AnimTrigger::OnClick);
  std::string x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:pRg st=\"0\" end=\"1\"/>"));
  CHECK(Has(x, "<p:pRg st=\"3\" end=\"3\"/>"));
  CHECK(Has(x, "<p:bldP spid=\"3\" grpId=\"0\" build=\"p\"/>"));
  show::Timing timing = show::ParseTiming(deck->GetSlide(0)->timing);
  CHECK(std::find(timing.byParagraph.begin(), timing.byParagraph.end(), 3u) != timing.byParagraph.end() || timing.main.steps.size() == 2);
  CHECK(!AddAnimation(ed, 0, 1, s));  // no such shape
}

TEST(SlideAnimOtherEffectsKeptByteForByte) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string fade =
      "<p:par><p:cTn id=\"9\" presetID=\"10\" presetClass=\"entr\" presetSubtype=\"0\" fill=\"hold\" grpId=\"0\" nodeType=\"withEffect\">"
      "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst><p:set><p:cBhvr><p:cTn id=\"10\" dur=\"1\" fill=\"hold\">"
      "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst></p:cTn><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl><p:attrNameLst>"
      "<p:attrName>style.visibility</p:attrName></p:attrNameLst></p:cBhvr><p:to><p:strVal val=\"visible\"/></p:to></p:set>"
      "<p:animEffect transition=\"in\" filter=\"fade\"><p:cBhvr><p:cTn id=\"11\" dur=\"500\"/><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl>"
      "</p:cBhvr></p:animEffect></p:childTnLst></p:cTn></p:par>";
  std::string builds = "<p:bldLst><p:bldP spid=\"2\" grpId=\"0\"/><p:bldP spid=\"3\" grpId=\"0\"/></p:bldLst>";
  CHECK(WriteSlide(ed, 0, std::string(kHead) + TimingWith(std::string(kBounce) + fade, builds) + "</p:sld>"));
  std::vector<AnimEffect> seq = MainSequence(ed, 0);
  CHECK_EQ(seq.size(), 2u);
  CHECK(seq[0].anim == Anim::Other && seq[0].cls == AnimClass::Entrance && !seq[0].editable && seq[0].shape == 2);
  CHECK(seq[0].durationMs == 1822);
  CHECK(seq[1].anim == Anim::Fade && seq[1].editable && seq[1].trigger == AnimTrigger::WithPrevious);

  // The fade's duration changes; the bounce stays as it was, byte for byte.
  AnimTimingPatch p;
  p.durationMs = 2000;
  CHECK(SetAnimationTiming(ed, 0, {1}, p));
  CHECK(Has(SlideXml(ed, 0), kBounce));
  // Its own duration and kind can't change; its trigger, delay and place can.
  CHECK(!SetAnimationTiming(ed, 0, {0}, p));
  CHECK(!SetAnimationOption(ed, 0, {0}, 0));
  AnimTimingPatch after;
  after.trigger = AnimTrigger::AfterPrevious;
  CHECK(SetAnimationTiming(ed, 0, {1}, after));
  std::string x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:cond delay=\"1822\"/>"));  // the fade's group starts when the bounce ends
  // Moved after the fade, it keeps its trigger: the fade starts with the slide, the bounce on a click.
  CHECK(MoveAnimations(ed, 0, {0}, 1));
  x = SlideXml(ed, 0);
  CHECK(Has(x, kBounce));
  CHECK(IdsUnique(x));
  seq = MainSequence(ed, 0);
  CHECK(seq[0].anim == Anim::Fade && seq[0].trigger == AnimTrigger::AfterPrevious);
  CHECK(seq[1].anim == Anim::Other && seq[1].trigger == AnimTrigger::OnClick);
  CHECK(RemoveAnimations(ed, 0, {1}));
  x = SlideXml(ed, 0);
  CHECK(!Has(x, "presetID=\"26\""));
  CHECK(!Has(x, "<p:bldP spid=\"2\""));
  CHECK(Has(x, "<p:bldP spid=\"3\" grpId=\"0\"/>"));
}

TEST(SlideAnimOddSequencesAreNotChanged) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  // A time group with an end condition: not PowerPoint's shape.
  std::string timing = TimingWith(kBounce, "");
  timing.replace(timing.find("<p:cTn id=\"4\" fill=\"hold\">"), 26, "<p:cTn id=\"4\" fill=\"hold\" restart=\"always\">");
  std::string slide = std::string(kHead) + timing + "</p:sld>";
  CHECK(WriteSlide(ed, 0, slide));
  std::u16string why;
  CHECK(MainSequence(ed, 0, &why).empty());
  CHECK(!why.empty());
  AnimSpec s;
  CHECK(!AddAnimation(ed, 0, 3, s));
  CHECK(!RemoveAnimations(ed, 0, {0}));
  std::string now = SlideXml(ed, 0);
  CHECK(now.substr(now.find("<p:sld")) == slide.substr(slide.find("<p:sld")));  // the declaration's line break aside
  CHECK(!ed.CanRedo() && ed.UndoName() == u"Write");
  // A slide with an empty timing (PowerPoint 2007 leaves one): animations can be added.
  CHECK(WriteSlide(ed, 1, std::string(kHead) +
                              "<p:timing><p:tnLst><p:par><p:cTn id=\"1\" dur=\"indefinite\" restart=\"never\" nodeType=\"tmRoot\"/>"
                              "</p:par></p:tnLst></p:timing></p:sld>"));
  CHECK(AddAnimation(ed, 1, 2, s));
  std::string x = SlideXml(ed, 1);
  CHECK_EQ(Count(x, "<p:timing>"), 1u);
  CHECK(Has(x, "nodeType=\"mainSeq\""));
  CHECK_EQ(MainSequence(ed, 1).size(), 1u);
  CHECK(IdsUnique(x));
}

// A video's play step in the main sequence (p3-present's Playback › Start), with its media node and the
// sequence that pauses it on a click, as PowerPoint writes them.
TEST(SlideAnimMediaPlayStepTakesItsStart) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  const std::string play =
      "<p:par><p:cTn id=\"5\" presetID=\"1\" presetClass=\"mediacall\" presetSubtype=\"0\" fill=\"hold\" nodeType=\"clickEffect\">"
      "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst><p:cmd type=\"call\" cmd=\"playFrom(0.0)\"><p:cBhvr>"
      "<p:cTn id=\"6\" dur=\"12345\" fill=\"hold\"/><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl></p:cBhvr></p:cmd>"
      "</p:childTnLst></p:cTn></p:par>";
  const std::string media =
      "<p:video><p:cMediaNode vol=\"80000\"><p:cTn id=\"7\" fill=\"hold\" display=\"0\"><p:stCondLst><p:cond delay=\"indefinite\"/>"
      "</p:stCondLst></p:cTn><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl></p:cMediaNode></p:video>"
      "<p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=\"8\" restart=\"whenNotActive\" fill=\"hold\" evtFilter=\"cancelBubble\" "
      "nodeType=\"interactiveSeq\"><p:stCondLst><p:cond evt=\"onClick\" delay=\"0\"><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl></p:cond>"
      "</p:stCondLst><p:endSync evt=\"end\" delay=\"0\"><p:rtn val=\"all\"/></p:endSync><p:childTnLst><p:par><p:cTn id=\"9\" fill=\"hold\">"
      "<p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst><p:par><p:cTn id=\"10\" fill=\"hold\"><p:stCondLst>"
      "<p:cond delay=\"0\"/></p:stCondLst><p:childTnLst><p:par><p:cTn id=\"11\" presetID=\"2\" presetClass=\"mediacall\" "
      "presetSubtype=\"0\" fill=\"hold\" nodeType=\"clickEffect\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>"
      "<p:cmd type=\"call\" cmd=\"togglePause\"><p:cBhvr><p:cTn id=\"12\" dur=\"1\" fill=\"hold\"/><p:tgtEl><p:spTgt spid=\"3\"/>"
      "</p:tgtEl></p:cBhvr></p:cmd></p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par>"
      "</p:childTnLst></p:cTn><p:nextCondLst><p:cond evt=\"onNextClick\" delay=\"0\"><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl></p:cond></p:nextCondLst></p:seq>";
  std::string timing = TimingWith(play, "");
  timing.insert(timing.find("</p:seq>") + 8, media);
  CHECK(WriteSlide(ed, 0, std::string(kHead) + timing + "</p:sld>"));
  std::u16string why;
  std::vector<AnimEffect> seq = MainSequence(ed, 0, &why);
  CHECK(why.empty());
  CHECK_EQ(seq.size(), 1u);
  if (seq.size() != 1) return;
  CHECK(seq[0].anim == Anim::Other && seq[0].cls == AnimClass::Other && !seq[0].editable && seq[0].shape == 3);
  CHECK(seq[0].presetId == 1 && seq[0].presetSubtype == 0);

  // Start › Automatically: the play step follows the slide's start; the media node and the pause
  // sequence stay as they were.
  AnimTimingPatch automatically;
  automatically.trigger = AnimTrigger::AfterPrevious;
  CHECK(SetAnimationTiming(ed, 0, {0}, automatically));
  std::string x = SlideXml(ed, 0);
  CHECK(Has(x, "presetClass=\"mediacall\" presetSubtype=\"0\" fill=\"hold\" nodeType=\"afterEffect\""));
  CHECK(Has(x, "evt=\"onBegin\""));
  CHECK(Has(x, media));
  CHECK(IdsUnique(x));
  seq = MainSequence(ed, 0);
  CHECK(!seq.empty() && seq[0].trigger == AnimTrigger::AfterPrevious);
  // An animation added after it gets new ids; then back to In Click Sequence.
  AnimSpec fade;
  CHECK(AddAnimation(ed, 0, 2, fade));
  AnimTimingPatch onClick;
  onClick.trigger = AnimTrigger::OnClick;
  CHECK(SetAnimationTiming(ed, 0, {0}, onClick));
  x = SlideXml(ed, 0);
  CHECK(Has(x, media));
  CHECK(!Has(x, "evt=\"onBegin\""));
  CHECK(IdsUnique(x));
  seq = MainSequence(ed, 0);
  CHECK_EQ(seq.size(), 2u);
  CHECK(seq.size() == 2 && seq[0].trigger == AnimTrigger::OnClick && seq[1].anim == Anim::Fade);
}

// Effect Options › Sequence: the body placeholder by paragraph and back, its kind, option and timing
// kept; a shape without text can't go by paragraph, and nothing changes then.
TEST(SlideAnimSequenceByParagraphAndBack) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  CHECK(WriteSlide(ed, 0, std::string(kHead) + "</p:sld>"));
  CHECK(AddAnimation(ed, 0, 2, AnimSpec{}));
  AnimSpec fly;
  fly.anim = Anim::FlyIn;
  fly.option = 2;
  fly.trigger = AnimTrigger::AfterPrevious;
  fly.delayMs = 250;
  CHECK(AddAnimation(ed, 0, 3, fly));

  CHECK(SetAnimationSequence(ed, 0, 1, true));
  CHECK(ed.UndoName() == u"Effect Options");
  std::vector<AnimEffect> seq = MainSequence(ed, 0);
  CHECK_EQ(seq.size(), 3u);  // "One" with "One.a" under it, and "Two"; the empty paragraph left out
  if (seq.size() != 3) return;
  CHECK(seq[1].shape == 3 && seq[1].paragraph == 0 && seq[1].anim == Anim::FlyIn && seq[1].option == 2);
  CHECK(seq[1].trigger == AnimTrigger::AfterPrevious && seq[1].delayMs == 250);
  CHECK(seq[2].shape == 3 && seq[2].paragraph == 3);
  std::string x = SlideXml(ed, 0);
  CHECK_EQ(Count(x, "<p:bldP spid=\"3\""), 1u);
  CHECK(Has(x, "build=\"p\""));
  CHECK(IdsUnique(x));

  // Back to one object, from the second paragraph's effect.
  CHECK(SetAnimationSequence(ed, 0, 2, false));
  seq = MainSequence(ed, 0);
  CHECK_EQ(seq.size(), 2u);
  if (seq.size() != 2) return;
  CHECK(seq[1].shape == 3 && seq[1].paragraph == -1 && seq[1].anim == Anim::FlyIn && seq[1].option == 2);
  CHECK(seq[1].trigger == AnimTrigger::AfterPrevious && seq[1].delayMs == 250);
  x = SlideXml(ed, 0);
  CHECK_EQ(Count(x, "<p:bldP spid=\"3\""), 1u);
  CHECK(!Has(x, "build=\"p\""));
  CHECK(IdsUnique(x));
  // Already one object: nothing to do.
  CHECK(SetAnimationSequence(ed, 0, 1, false));

  // A shape without text: refused, and the slide as it was.
  uint32_t rect = 0;
  CHECK(ed.InsertAutoShape(0, "rect", {100, 100, 200, 160}, &rect));
  CHECK(AddAnimation(ed, 0, rect, AnimSpec{}));
  const std::string before = SlideXml(ed, 0);
  CHECK(!SetAnimationSequence(ed, 0, 2, true));
  CHECK(SlideXml(ed, 0) == before);
  CHECK(ed.UndoName() == u"Add Animation");
}

// A layout change that drops an empty placeholder takes its effects and build with it (p3-corpus's
// torture: new slide, an effect on its empty title, then a layout without a title).
TEST(SlideAnimLayoutChangeDropsTheEffectsOfDroppedPlaceholders) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  size_t title = SIZE_MAX, blank = SIZE_MAX;
  for (size_t i = 0; i < deck->Layouts().size(); ++i) {
    if (deck->Layouts()[i]->type == "title") title = i;
    if (deck->Layouts()[i]->type == "blank") blank = i;
  }
  CHECK(title != SIZE_MAX && blank != SIZE_MAX);
  if (title == SIZE_MAX || blank == SIZE_MAX) return;
  const size_t at = deck->SlideCount();
  CHECK(ed.NewSlide(at, title));
  const Slide* slide = deck->GetSlide(at);
  CHECK(slide && !slide->shapes.empty());
  if (!slide || slide->shapes.empty()) return;
  const uint32_t shape = slide->shapes.front().id;
  AnimSpec floatOut;
  floatOut.anim = Anim::FloatOut;
  floatOut.option = 1;
  CHECK(AddAnimation(ed, at, shape, floatOut));
  CHECK(Has(SlideXml(ed, at), "<p:spTgt spid=\"" + std::to_string(shape) + "\""));
  CHECK(ed.SetLayout({at}, blank));
  const std::string x = SlideXml(ed, at);
  CHECK(!Has(x, "spid=\"" + std::to_string(shape) + "\""));
  CHECK(MainSequence(ed, at).empty());
  CHECK(ed.Undo());
  CHECK_EQ(MainSequence(ed, at).size(), 1u);
}

// PowerPoint's Duration is one play; Repeat is apart (p3-corpus's read-back: a path repeated twice).
TEST(SlideAnimDurationIsOnePlay) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  std::string fade =
      "<p:par><p:cTn id=\"5\" presetID=\"10\" presetClass=\"entr\" presetSubtype=\"0\" repeatCount=\"2000\" fill=\"hold\" grpId=\"0\" "
      "nodeType=\"clickEffect\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst><p:set><p:cBhvr><p:cTn id=\"6\" dur=\"1\" "
      "fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst></p:cTn><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl><p:attrNameLst>"
      "<p:attrName>style.visibility</p:attrName></p:attrNameLst></p:cBhvr><p:to><p:strVal val=\"visible\"/></p:to></p:set>"
      "<p:animEffect transition=\"in\" filter=\"fade\"><p:cBhvr><p:cTn id=\"7\" dur=\"500\"/><p:tgtEl><p:spTgt spid=\"3\"/></p:tgtEl>"
      "</p:cBhvr></p:animEffect></p:childTnLst></p:cTn></p:par>";
  CHECK(WriteSlide(ed, 0, std::string(kHead) + TimingWith(fade, "<p:bldLst><p:bldP spid=\"3\" grpId=\"0\"/></p:bldLst>") + "</p:sld>"));
  std::vector<AnimEffect> seq = MainSequence(ed, 0);
  CHECK_EQ(seq.size(), 1u);
  if (seq.size() != 1) return;
  CHECK(seq[0].durationMs == 500 && seq[0].repeat == 2);
  AnimTimingPatch p;
  p.durationMs = 1000;
  CHECK(SetAnimationTiming(ed, 0, {0}, p));
  seq = MainSequence(ed, 0);
  CHECK(seq.size() == 1 && seq[0].durationMs == 1000 && seq[0].repeat == 2);
  CHECK(Has(SlideXml(ed, 0), "<p:cTn id=\"7\" dur=\"1000\"/>"));
}

// A slide without a transition of its own plays its layout's (p3-corpus's read-back: PowerPoint's
// SlideShowTransition reads it). Changing it writes the slide its own; None over it is an empty one.
TEST(SlideAnimTransitionFromTheLayout) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  const std::string layoutPart = deck->GetSlide(0)->layout ? deck->GetSlide(0)->layout->part : std::string();
  CHECK(!layoutPart.empty());
  if (layoutPart.empty()) return;
  std::vector<uint8_t> b;
  CHECK(ed.ReadPart(layoutPart, b));
  std::string layout(b.begin(), b.end());
  const size_t at = layout.find("<p:clrMapOvr>") != std::string::npos ? layout.find("<p:clrMapOvr>") : layout.find("</p:sldLayout>");
  // After clrMapOvr if there is one: transition comes after it in the schema.
  size_t place = layout.find("</p:clrMapOvr>");
  place = place != std::string::npos ? place + 14 : at;
  layout.insert(place, "<p:transition spd=\"slow\"><p:wipe/></p:transition>");
  CHECK(ed.Run(u"Write", [&] {
    return ed.WritePart(layoutPart, std::vector<uint8_t>(layout.begin(), layout.end())) ? Result{} : Result::Fail(u"write");
  }));
  CHECK(!Has(SlideXml(ed, 0), "<p:transition"));

  TransitionInfo t = GetTransition(ed, 0);
  CHECK(t.inherited && t.effect == TransitionEffect::Wipe && t.durationMs == 1000);
  CHECK(show::ParseTransition(deck->GetSlide(0)->transition).durationMs == 1000);  // the show plays it

  // A new duration: the slide gets its own wipe.
  TransitionPatch longer;
  longer.durationMs = 2000;
  CHECK(SetTransition(ed, {0}, longer));
  t = GetTransition(ed, 0);
  CHECK(!t.inherited && t.effect == TransitionEffect::Wipe && t.durationMs == 2000);
  CHECK(Has(SlideXml(ed, 0), "<p:wipe"));

  // None over the layout's: an empty transition of the slide's own, and nothing plays.
  TransitionPatch none;
  none.effect = TransitionEffect::None;
  CHECK(SetTransition(ed, {0}, none));
  t = GetTransition(ed, 0);
  CHECK(!t.inherited && t.effect == TransitionEffect::None);
  const std::string x = SlideXml(ed, 0);
  CHECK(Has(x, "<p:transition") && !Has(x, "<p:wipe"));
  CHECK(show::ParseTransition(deck->GetSlide(0)->transition).kind == show::TransitionKind::None);

  // Apply To All from a slide playing the layout's: every slide gets its own copy.
  CHECK(ed.Undo(2));
  CHECK(GetTransition(ed, 0).inherited);
  CHECK(ApplyTransitionToAll(ed, 0));
  for (size_t i = 1; i < deck->SlideCount(); ++i) {
    t = GetTransition(ed, i);
    CHECK(!t.inherited && t.effect == TransitionEffect::Wipe);
  }
}

// Header & Footer turning slide numbers off drops the number's placeholder with its effects and build
// (p3-corpus's torture: an effect on a slide-number placeholder, then Header & Footer without it).
TEST(SlideAnimHeaderFooterDropsTheEffectsOfDroppedPlaceholders) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  Editor ed(*deck);
  size_t layout = SIZE_MAX;
  for (size_t i = 0; i < deck->Layouts().size(); ++i)
    if (deck->Layouts()[i]->type == "obj") layout = i;
  CHECK(layout != SIZE_MAX);
  if (layout == SIZE_MAX) return;
  const size_t at = deck->SlideCount();
  CHECK(ed.NewSlide(at, layout));
  HeaderFooter numbers;
  numbers.slideNumber = true;
  CHECK(ed.SetHeaderFooter({at}, numbers));
  uint32_t number = 0;
  for (const Shape& sh : deck->GetSlide(at)->shapes)
    if (sh.placeholder.type == PlaceholderType::SlideNumber) number = sh.id;
  CHECK(number != 0);
  if (!number) return;
  AnimSpec fade;
  CHECK(AddAnimation(ed, at, number, fade));
  CHECK_EQ(MainSequence(ed, at).size(), 1u);
  CHECK(ed.SetHeaderFooter({at}, HeaderFooter{}));
  const std::string x = SlideXml(ed, at);
  CHECK(!Has(x, "spid=\"" + std::to_string(number) + "\""));
  CHECK(MainSequence(ed, at).empty());
}
