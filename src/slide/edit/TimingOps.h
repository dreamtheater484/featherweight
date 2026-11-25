#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "slide/edit/Edit.h"

// Transitions and animations (P3, p3-anim): a slide's p:transition and the main sequence of its
// p:timing, written as PowerPoint 2021 writes them. Each call is one undo step (or part of an open
// group) and changes nothing when it fails, as Editor's own operations.
//
// What Featherweight can't change is kept as it is: transitions other than the common ones (§8.2),
// animations other than the common ones (their behaviors stay byte for byte, also when other
// effects on the slide change, and they can still be moved, retimed or removed), sounds, and the
// sequences shapes trigger. A main sequence not stored the way PowerPoint stores it is shown and
// played, never changed (MainSequence says why).
namespace fw::slide::edit {

// ---- Transitions (the Transitions tab)

enum class TransitionEffect : uint8_t {
  None,
  Cut,
  Fade,
  Push,
  Wipe,
  Split,
  Uncover,
  Cover,
  Zoom,
  Other,  // one Featherweight can't set (Morph, Vortex...): kept, its timing can still change
};

const char16_t* TransitionName(TransitionEffect effect);
// The Effect Options menu, in PowerPoint's order ("From Bottom"...); empty when there are none.
std::vector<std::u16string> TransitionOptions(TransitionEffect effect);
// PowerPoint's duration when the effect is picked (ms); 0 for None and Cut.
double DefaultTransitionMs(TransitionEffect effect);

struct TransitionInfo {
  TransitionEffect effect = TransitionEffect::None;
  int option = 0;              // index into TransitionOptions(effect)
  double durationMs = 0;       // 0 for None and Cut
  bool advanceOnClick = true;
  double advanceAfterMs = -1;  // -1: the slide doesn't advance by itself
  bool sound = false;          // a sound plays with it (kept; not changed here)
  std::string element;         // Other: the effect's element as written, "p15:prstTrans prst=curtains"
  bool inherited = false;      // the slide has none of its own and plays its layout's (or master's), as
                               // PowerPoint does; changing it writes the slide its own
};
TransitionInfo GetTransition(const Editor& ed, size_t slide);

// Each set field changes; the rest stay. A new effect takes its default option and duration
// unless they're given (as picking one in PowerPoint's gallery does).
struct TransitionPatch {
  std::optional<TransitionEffect> effect;  // not Other
  std::optional<int> option;
  std::optional<double> durationMs;        // 10 ms to 59 s
  std::optional<bool> advanceOnClick;
  std::optional<double> advanceAfterMs;    // < 0: off; up to 24 hours
};
Result SetTransition(Editor& ed, const std::vector<size_t>& slides, const TransitionPatch& patch);
// Apply To All: every slide gets slide `from`'s transition (effect, options, duration, advance, sound).
Result ApplyTransitionToAll(Editor& ed, size_t from);

// ---- Animations (the Animations tab and the Animation Pane)

enum class Anim : uint8_t {
  // Entrance
  Appear, Fade, FlyIn, FloatIn, Split, Wipe, Zoom,
  // Emphasis
  GrowShrink, Spin,
  // Exit
  Disappear, FadeOut, FlyOut, FloatOut, SplitOut, WipeOut, ZoomOut,
  Other,  // one Featherweight can't change (kept as it is)
};
enum class AnimClass : uint8_t { Entrance, Emphasis, Exit, Path, Other };

AnimClass ClassOf(Anim anim);
const char16_t* AnimName(Anim anim);  // "Fly In" (exits by their own name: "Fly Out")
std::vector<std::u16string> AnimOptions(Anim anim);  // the Effect Options menu's directions
int DefaultAnimOption(Anim anim);
double DefaultAnimMs(Anim anim);  // 0 for Appear and Disappear

enum class AnimTrigger : uint8_t { OnClick, WithPrevious, AfterPrevious };

// One effect of the main sequence, in the order the Animation Pane lists them.
struct AnimEffect {
  uint32_t shape = 0;  // the shape it animates (cNvPr id); 0 when it animates no shape (a sound)
  int paragraph = -1;  // the first paragraph of its text it animates, -1: the whole shape
  Anim anim = Anim::Other;
  AnimClass cls = AnimClass::Other;  // also for Other (from presetClass)
  int option = 0;                    // index into AnimOptions(anim)
  AnimTrigger trigger = AnimTrigger::OnClick;
  double durationMs = 0;  // how long it plays once (0: at once), PowerPoint's Duration
  double repeat = 1;      // how many times (repeatCount); 0: until the end of the slide
  double delayMs = 0;
  bool editable = false;  // its kind, options and duration can change (a common animation as PowerPoint writes it)
  int presetId = 0, presetSubtype = 0;  // as written (with cls, what an Other effect is: 26/0 is Bounce)
};

// The slide's main sequence, empty when it has none. `whyNot` gets why it can't be changed, empty
// when it can.
std::vector<AnimEffect> MainSequence(const Editor& ed, size_t slide, std::u16string* whyNot = nullptr);

struct AnimSpec {
  Anim anim = Anim::Fade;  // not Other
  int option = -1;         // -1: its default
  std::optional<double> durationMs;  // its default when not set
  AnimTrigger trigger = AnimTrigger::OnClick;
  double delayMs = 0;
  // The shape's text one paragraph at a time (each first-level paragraph with the ones below it,
  // each after the first on its own click), as PowerPoint's Sequence > By Paragraph.
  bool byParagraph = false;
};
// Adds an animation of `shape` (on the slide itself, not inside a group) at `at` in the main
// sequence (-1: the end). `index` gets the first new effect's index.
Result AddAnimation(Editor& ed, size_t slide, uint32_t shape, const AnimSpec& spec, int at = -1,
                    size_t* index = nullptr);
// Effects (indexes into MainSequence) become `anim`, with its default option and duration (the
// gallery with effects selected); their triggers, delays and order stay.
Result ChangeAnimation(Editor& ed, size_t slide, const std::vector<size_t>& effects, Anim anim);
Result SetAnimationOption(Editor& ed, size_t slide, const std::vector<size_t>& effects, int option);

struct AnimTimingPatch {
  std::optional<AnimTrigger> trigger;
  std::optional<double> durationMs;  // 10 ms to 59 s; only effects that have a duration
  std::optional<double> delayMs;     // 0 to 59 s
};
Result SetAnimationTiming(Editor& ed, size_t slide, const std::vector<size_t>& effects, const AnimTimingPatch& patch);
// `effects` (kept in their order) moved so the first lands at index `to` of the sequence without
// them (Move Earlier and Move Later, dragging in the Animation Pane). Triggers stay.
Result MoveAnimations(Editor& ed, size_t slide, const std::vector<size_t>& effects, size_t to);
Result RemoveAnimations(Editor& ed, size_t slide, const std::vector<size_t>& effects);
// Effect Options › Sequence (As One Object, By Paragraph): the shape of `effect` animated as one
// object, or a paragraph at a time. Its effects of that kind in a row become one effect per
// paragraph, or one for the whole shape, with the first one's kind, option and timing. One undo step;
// nothing changes when it fails (a shape without text can't go by paragraph).
Result SetAnimationSequence(Editor& ed, size_t slide, size_t effect, bool byParagraph);

}  // namespace fw::slide::edit
