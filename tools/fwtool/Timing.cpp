// fwtool timing (P3, p3-anim): each slide's transition and main animation sequence as the edit layer
// reads them (src/slide/edit/TimingOps.h), one line each, for comparing a deck Featherweight saved
// with PowerPoint's own save of it (p3-corpus's read-back check) and for the torture gate's logs.
#include <cstdio>
#include <string>

#include "Common.h"
#include "core/Strings.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
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

}  // namespace

int RunTiming(const Options& options) {
  namespace ed = fw::slide::edit;
  int failures = 0;
  for (const auto& file : CollectFiles(options.inputs)) {
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
      std::printf(" %.0f ms, click %d, after %.0f ms%s\n", t.durationMs, t.advanceOnClick ? 1 : 0, t.advanceAfterMs,
                  t.sound ? ", sound" : "");
      if (!why.empty()) std::printf("    animations not editable: %s\n", U8(why).c_str());
      for (size_t k = 0; k < seq.size(); ++k) {
        const ed::AnimEffect& a = seq[k];
        std::printf("    %zu. shape %u", k + 1, a.shape);
        if (a.paragraph >= 0) std::printf(" paragraph %d", a.paragraph);
        std::printf(" %s %s", ClassName(a.cls), U8(ed::AnimName(a.anim)).c_str());
        std::vector<std::u16string> ao = ed::AnimOptions(a.anim);
        if (a.anim != ed::Anim::Other && a.option >= 0 && a.option < static_cast<int>(ao.size()))
          std::printf(" (%s)", U8(ao[a.option]).c_str());
        std::printf(", %s, %.0f ms, delay %.0f ms%s\n", TriggerName(a.trigger), a.durationMs, a.delayMs,
                    a.editable ? "" : ", kept");
      }
    }
  }
  return failures ? 1 : 0;
}

}  // namespace fwtool
