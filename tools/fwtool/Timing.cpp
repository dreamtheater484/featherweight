// fwtool timing (P3, p3-anim): each slide's transition and main animation sequence as the edit layer
// reads them (src/slide/edit/TimingOps.h), one line each, for comparing a deck Featherweight saved
// with PowerPoint's own save of it (p3-corpus's read-back check) and for the torture gate's logs.
// With --out <dir>: first makes the read-back check's deck from each deck given (MakeTimingDeck),
// <dir>/<name>-timing.pptx, and lists those.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Common.h"
#include "core/Strings.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Save.h"
#include "slide/edit/TimingOps.h"

namespace fwtool {
namespace {

std::string U8(std::u16string_view s) { return fw::WideToUtf8(std::wstring(s.begin(), s.end())); }

const char* TriggerName(fw::slide::edit::AnimTrigger t) {
  using fw::slide::edit::AnimTrigger;
  return t == AnimTrigger::OnClick ? "click" : t == AnimTrigger::WithPrevious ? "with" : "after";
}

const char* ClassName(fw::slide::edit::AnimClass c) {
  using fw::slide::edit::AnimClass;
  return c == AnimClass::Entrance ? "entr" : c == AnimClass::Emphasis ? "emph" : c == AnimClass::Exit ? "exit"
                                                                       : c == AnimClass::Path ? "path" : "other";
}

// The read-back check's deck, made through the edit layer after the deck's own slides: a slide per
// common transition and option (durations at and off the nominal speeds, some advancing after a
// time, one without a click); then every common animation and option, six to a slide on text boxes
// and a shape without text, the three triggers, durations and delays varied; a list by paragraph;
// and a slide whose effects were added, changed, retimed, moved and removed. Saved as `out`; false
// when an edit or the save failed.
bool MakeTimingDeck(const std::filesystem::path& file, const std::filesystem::path& out) {
  namespace ed = fw::slide::edit;
  fw::pkg::Status st;
  auto pkg = OpenPackage(file, st);
  std::unique_ptr<fw::slide::Presentation> deck = pkg ? fw::slide::Read(std::move(pkg), st) : nullptr;
  if (!deck || deck->Layouts().empty()) {
    std::printf("  refused: %s\n", st.message.c_str());
    return false;
  }
  ed::Editor editor(*deck);
  size_t blank = 0;
  for (size_t i = 0; i < deck->Layouts().size(); ++i)
    if (deck->Layouts()[i]->type == "blank") blank = i;
  int failures = 0;
  auto check = [&](const ed::Result& r, const std::string& what) {
    if (!r) {
      std::printf("  %s: %s\n", what.c_str(), U8(r.message).c_str());
      ++failures;
    }
  };
  size_t at = deck->SlideCount();
  auto newSlide = [&](const std::wstring& title) {
    check(editor.NewSlide(at, blank), "new slide");
    check(editor.InsertTextBox(at, {36, 12, 900, 40}, title), "title");
    return at++;
  };

  using TE = ed::TransitionEffect;
  const TE effects[] = {TE::Cut, TE::Fade, TE::Push, TE::Wipe, TE::Split, TE::Uncover, TE::Cover, TE::Zoom};
  const double offNominal[] = {500, 1250, 750, 300, 1000, 2000, 1600};
  int n = 0;
  for (TE e : effects) {
    const size_t options = std::max<size_t>(ed::TransitionOptions(e).size(), 1);
    for (size_t o = 0; o < options; ++o, ++n) {
      const std::u16string name = ed::TransitionName(e);
      const size_t slide = newSlide(L"Transition " + std::wstring(name.begin(), name.end()) + L" " + std::to_wstring(o));
      ed::TransitionPatch p;
      p.effect = e;
      p.option = static_cast<int>(o);
      if (e != TE::Cut && n % 2 == 1) p.durationMs = offNominal[n % 7];
      if (n % 4 == 3) {
        p.advanceAfterMs = 1000 + 500 * (n % 5);
        if (n % 8 == 7) p.advanceOnClick = false;
      }
      check(ed::SetTransition(editor, {slide}, p), "transition " + U8(name));
    }
  }

  using A = ed::Anim;
  const A anims[] = {A::Appear,    A::Fade, A::FlyIn,     A::FloatIn, A::Split,  A::Wipe,     A::Zoom,     A::GrowShrink,
                     A::Spin,      A::Disappear, A::FadeOut, A::FlyOut, A::FloatOut, A::SplitOut, A::WipeOut, A::ZoomOut};
  struct One {
    A anim;
    int option;
  };
  std::vector<One> all;
  for (A a : anims)
    for (size_t o = 0; o < std::max<size_t>(ed::AnimOptions(a).size(), 1); ++o) all.push_back({a, static_cast<int>(o)});
  size_t slide = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    if (i % 6 == 0) slide = newSlide(L"Animations " + std::to_wstring(i / 6 + 1));
    const float y = 60 + 70 * static_cast<float>(i % 6);
    const std::u16string name = ed::AnimName(all[i].anim);
    uint32_t shape = 0;
    if (i % 6 == 5) {
      check(editor.InsertAutoShape(slide, "rect", {500, y, 620, y + 50}, &shape), "shape");
    } else {
      const std::wstring label = std::wstring(name.begin(), name.end()) + L" " + std::to_wstring(all[i].option);
      check(editor.InsertTextBox(slide, {40, y, 440, y + 50}, label, &shape), "text box");
    }
    ed::AnimSpec s;
    s.anim = all[i].anim;
    s.option = all[i].option;
    s.trigger = i % 3 == 0   ? ed::AnimTrigger::OnClick
                : i % 3 == 1 ? ed::AnimTrigger::WithPrevious
                             : ed::AnimTrigger::AfterPrevious;
    if (ed::DefaultAnimMs(all[i].anim) > 0 && i % 4 == 1) s.durationMs = 1500;
    if (i % 5 == 2) s.delayMs = 250;
    check(ed::AddAnimation(editor, slide, shape, s), "animation " + U8(name));
  }

  // By paragraph: three paragraphs flying in, one click each.
  slide = newSlide(L"By paragraph");
  uint32_t list = 0;
  check(editor.InsertTextBox(slide, {40, 80, 600, 260}, L"First\nSecond\nThird", &list), "list");
  ed::AnimSpec byParagraph;
  byParagraph.anim = A::FlyIn;
  byParagraph.byParagraph = true;
  check(ed::AddAnimation(editor, slide, list, byParagraph), "by paragraph");

  // Edited after adding: changed, given options and timing, moved and removed.
  slide = newSlide(L"Edited");
  for (int k = 0; k < 4; ++k) {
    uint32_t shape = 0;
    const float y = 80 + 70.0f * static_cast<float>(k);
    check(editor.InsertTextBox(slide, {40, y, 440, y + 50}, L"Shape " + std::to_wstring(k + 1), &shape), "text box");
    ed::AnimSpec s;
    s.anim = A::Fade;
    check(ed::AddAnimation(editor, slide, shape, s), "fade");
  }
  check(ed::ChangeAnimation(editor, slide, {0}, A::FlyIn), "change");
  check(ed::SetAnimationOption(editor, slide, {0}, 2), "option");
  ed::AnimTimingPatch timing;
  timing.trigger = ed::AnimTrigger::AfterPrevious;
  timing.durationMs = 1200;
  timing.delayMs = 400;
  check(ed::SetAnimationTiming(editor, slide, {1}, timing), "timing");
  check(ed::MoveAnimations(editor, slide, {3}, 0), "move");
  check(ed::RemoveAnimations(editor, slide, {3}), "remove");

  std::vector<uint8_t> bytes;
  const fw::pkg::Status saved = ed::SaveToBytes(editor, bytes);
  if (!saved.Ok()) {
    std::printf("  save: %s\n", saved.message.c_str());
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(out.parent_path(), ec);
  std::ofstream(out, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  std::printf("  made %s (%zu slides)\n", Display(out).c_str(), deck->SlideCount());
  return failures == 0;
}

}  // namespace

int RunTiming(const Options& options) {
  namespace ed = fw::slide::edit;
  int failures = 0;
  auto files = CollectFiles(options.inputs);
  if (!options.outDir.empty()) {
    decltype(files) made;
    for (const auto& file : files) {
      std::printf("%s\n", Display(file).c_str());
      const std::filesystem::path out = options.outDir / (file.stem().wstring() + L"-timing.pptx");
      if (!MakeTimingDeck(file, out)) ++failures;
      if (std::filesystem::exists(out)) made.push_back(out);
    }
    files = made;
  }
  for (const auto& file : files) {
    const std::wstring ext = fw::ToLower(file.extension().wstring());
    if (!ext.starts_with(L".ppt") && !ext.starts_with(L".pot") && !ext.starts_with(L".pps")) continue;
    std::printf("%s\n", Display(file).c_str());
    fw::pkg::Status st;
    auto pkg = OpenPackage(file, st);
    std::unique_ptr<fw::slide::Presentation> deck = pkg ? fw::slide::Read(std::move(pkg), st) : nullptr;
    if (!deck) {
      std::printf("  refused: %s\n", st.message.c_str());
      ++failures;
      continue;
    }
    ed::Editor editor(*deck);
    for (size_t i = 0; i < deck->SlideCount(); ++i) {
      ed::TransitionInfo t = ed::GetTransition(editor, i);
      std::u16string why;
      std::vector<ed::AnimEffect> seq = ed::MainSequence(editor, i, &why);
      if (t.effect == ed::TransitionEffect::None && t.advanceOnClick && t.advanceAfterMs < 0 && seq.empty() && why.empty()) continue;
      std::printf("  slide %zu: transition %s", i + 1, U8(ed::TransitionName(t.effect)).c_str());
      std::vector<std::u16string> opts = ed::TransitionOptions(t.effect);
      if (t.option >= 0 && t.option < static_cast<int>(opts.size()))
        std::printf(" (%s)", U8(opts[t.option]).c_str());
      if (!t.element.empty()) std::printf(" <%s>", t.element.c_str());
      std::printf(" %.0f ms, click %d, after %.0f ms%s%s\n", t.durationMs, t.advanceOnClick ? 1 : 0, t.advanceAfterMs,
                  t.sound ? ", sound" : "", t.inherited ? ", from the layout" : "");
      if (!why.empty()) std::printf("    animations not editable: %s\n", U8(why).c_str());
      for (size_t k = 0; k < seq.size(); ++k) {
        const ed::AnimEffect& a = seq[k];
        std::printf("    %zu. shape %u", k + 1, a.shape);
        if (a.paragraph >= 0) std::printf(" paragraph %d", a.paragraph);
        std::printf(" %s %s", ClassName(a.cls), U8(ed::AnimName(a.anim)).c_str());
        std::vector<std::u16string> ao = ed::AnimOptions(a.anim);
        if (a.anim != ed::Anim::Other && a.option >= 0 && a.option < static_cast<int>(ao.size()))
          std::printf(" (%s)", U8(ao[a.option]).c_str());
        std::printf(", %s, %.0f ms, delay %.0f ms", TriggerName(a.trigger), a.durationMs, a.delayMs);
        if (a.repeat == 0) std::printf(", repeat until the end of the slide");
        else if (a.repeat != 1) std::printf(", repeat %g", a.repeat);
        if (!a.editable) std::printf(", kept #%d/%d", a.presetId, a.presetSubtype);
        std::printf("\n");
      }
    }
  }
  return failures ? 1 : 0;
}

}  // namespace fwtool
