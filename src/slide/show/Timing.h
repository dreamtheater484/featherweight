#pragma once

#include <pugixml.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The slide show's reading of a slide's <p:transition> and <p:timing> (ECMA-376 Part 1, §19.3.1.x and
// §19.5), and the interpreter for the common subset of PowerPoint's timing tree (plan §8.2, §8.3).
// Headless: no window, no Direct2D. The slide model (p1-model) keeps each slide's DOM; this reads its
// nodes as they are, mc:AlternateContent included.
namespace fw::slide::show {

// ---------------------------------------------------------------------------------------------
// Transitions

enum class TransitionKind : uint8_t {
  None,     // the next slide simply replaces this one
  Fade,     // also every transition played as a fade (plan §8.2: "others play as a fade")
  Push,
  Wipe,
  Split,
  Cover,
  Uncover,  // <p:pull>
  Zoom,
};

// Where a transition comes from or goes to. Push, Wipe, Cover and Uncover use the sides (and the
// corners for Cover and Uncover); Zoom uses In and Out.
enum class Side : uint8_t { Left, Up, Right, Down, LeftUp, RightUp, LeftDown, RightDown };

struct Transition {
  TransitionKind kind = TransitionKind::None;
  Side side = Side::Left;     // ECMA's dir: the direction the motion goes ("l": towards the left)
  bool vertical = false;      // Split: orient="vert"
  bool in = false;            // Split: dir="in" (from the edges to the middle); Zoom: dir="in"
  bool throughBlack = false;  // Fade thruBlk="1"
  bool played = true;         // false: an unknown transition played as a fade
  double durationMs = 0;
  bool advanceOnClick = true;
  double advanceAfterMs = -1;  // advTm, or -1 when the slide doesn't advance by itself
};

// `node` is the slide's <p:transition>, or an mc:AlternateContent around transitions (PowerPoint
// writes a p14 Choice with p14:dur and a plain Fallback). A null node is no transition.
Transition ParseTransition(pugi::xml_node node);

// ---------------------------------------------------------------------------------------------
// Animation targets and poses

// What a behavior animates: a whole shape, its background (fill and outline, apart from its text)
// or a range of its paragraphs.
struct TargetKey {
  enum Part : uint8_t { Whole, Background, Paragraphs };
  uint32_t spid = 0;  // the shape's cNvPr id
  Part part = Whole;
  int first = 0, last = 0;  // Paragraphs: the range, inclusive

  bool operator==(const TargetKey& o) const {
    return spid == o.spid && part == o.part && (part != Paragraphs || (first == o.first && last == o.last));
  }
  bool operator<(const TargetKey& o) const;
  bool Contains(int paragraph) const { return part == Paragraphs && paragraph >= first && paragraph <= last; }
};

// Where a target is on the slide, in fractions of the slide's width and height: what the timing
// formulas call ppt_x and ppt_y (the center) and ppt_w and ppt_h.
struct TargetBox {
  double x = 0.5, y = 0.5, w = 0, h = 0;
};

// How much of a target a wipe or a split shows.
struct Reveal {
  enum Kind : uint8_t { None, Wipe, Split };
  Kind kind = None;
  // Wipe: the edge the shown part starts at. PowerPoint's "Wipe From Bottom" is filter="wipe(down)":
  // Down, shown from the bottom edge up.
  Side side = Side::Down;
  bool vertical = false;   // Split: the split line is vertical (barn ...Vertical)
  bool outward = true;     // Split: from the middle out (barn out...)
  float shown = 1;         // 0: nothing, 1: all
};

// A target's state at one moment of the show. Everything is relative to the target as the slide
// shows it without animations.
struct Pose {
  bool visible = true;
  float opacity = 1;
  float dx = 0, dy = 0;  // offset of the center, in fractions of the slide
  float sx = 1, sy = 1;  // scale about the center
  float rotation = 0;    // degrees, clockwise
  Reveal reveal;

  bool Identity() const {
    return visible && opacity == 1 && dx == 0 && dy == 0 && sx == 1 && sy == 1 && rotation == 0 &&
           reveal.kind == Reveal::None;
  }
};

// ---------------------------------------------------------------------------------------------
// The timing tree, compiled into steps

enum class EffectClass : uint8_t { None, Entrance, Exit, Emphasis, Path, Verb, Media };

// One animated property over time (a <p:set>, <p:anim>, <p:animEffect>, <p:animScale>, <p:animRot>
// or <p:animMotion>), with its times resolved relative to the start of its step.
struct Behavior {
  enum Kind : uint8_t { Set, Anim, Effect, Scale, Rotate, Motion };
  enum Attr : uint8_t { Visibility, Opacity, X, Y, W, H, Rotation, Other };
  struct Key {
    double time = 0;  // 0..1 of the duration
    std::string value;
    std::string formula;  // tav fmla: $ is the value
  };

  Kind kind = Set;
  Attr attr = Other;
  TargetKey target;
  EffectClass effectClass = EffectClass::None;
  double begin = 0;        // ms after the step starts
  double duration = 0;     // ms of one iteration
  double repeat = 1;       // iterations (repeatCount / 1000)
  bool autoReverse = false;
  double accel = 0, decel = 0;  // fractions of the iteration
  bool hold = true;        // fill hold or freeze: the end value stays; false: removed at the end
  bool discrete = false;   // calcmode discrete, or string values
  bool additive = false;   // additive="sum": added to the base value
  std::vector<Key> keys;   // Set: one key, the "to" value; Anim: the tav list (or from/to/by)
  // Effect: the animEffect's filter, transition in or out.
  Reveal::Kind reveal = Reveal::None;
  bool fade = false;
  bool effectOut = false;
  Side side = Side::Down;
  bool vertical = false, outward = true;
  // Scale (x and y, fractions: 1 = 100 %), Rotate (degrees, in [0]): from/to, or by.
  double from[2] = {1, 1}, to[2] = {1, 1}, by[2] = {1, 1};
  bool hasFrom = false, hasTo = false, hasBy = false;
  // Motion: the path as points in slide fractions, relative to the target, already flattened.
  std::vector<std::pair<float, float>> path;

  double End() const;  // ms after the step starts
};

// What starts a step.
enum class StepStart : uint8_t { Click, WithPrevious, AfterPrevious };

// One click of the show: every behavior it starts, until the next click.
struct Step {
  StepStart start = StepStart::Click;
  double length = 0;  // ms until its last behavior ends
  std::vector<Behavior> behaviors;
};

// A sequence of steps: the main sequence, or one started by clicking a shape (a trigger).
struct Sequence {
  uint32_t triggerSpid = 0;  // 0: the main sequence
  std::vector<Step> steps;
};

struct Timing {
  Sequence main;
  std::vector<Sequence> triggers;
  // Targets hidden when the slide starts: their first effect is an entrance.
  std::vector<TargetKey> initiallyHidden;
  // Every target any behavior animates, sorted.
  std::vector<TargetKey> targets;
  // Shapes whose text is built by paragraph (bldP build="p" and similar), sorted.
  std::vector<uint32_t> byParagraph;
  int unsupportedEffects = 0;  // played as appear (or not at all, for emphasis)
};

// Reads <p:timing>. A null node gives an empty timing. Never fails: what it can't read is left out.
Timing ParseTiming(pugi::xml_node timing);

// Evaluates a timing formula ("#ppt_x+.1", "0-#ppt_w/2", "sin(pi*$)"). `value` is $; NaN when it
// can't be read.
double EvalFormula(std::string_view text, const TargetBox& box, double value = 0);

// ---------------------------------------------------------------------------------------------
// Playing a slide's timing

// The poses of a slide's targets as the show goes: steps start on clicks (or after the previous
// one), and Pose() says where everything is at a given time within the running step.
//
//   Animator a(timing, boxOf);
//   a.Start();                   // the slide appears: initial poses
//   a.Click(now);                // the next step starts at `now`
//   a.Update(now); a.PoseOf(k);  // during a step
//   a.Running(now)               // false once nothing moves: stop drawing frames
class Animator {
 public:
  using BoxFn = TargetBox (*)(void* ctx, const TargetKey& key);

  Animator() = default;
  Animator(const Timing* timing, BoxFn box, void* ctx) : timing_(timing), box_(box), ctx_(ctx) {}

  // Back to the slide's start (every step undone). With `begin`, steps that start with the slide
  // begin now; else BeginAuto starts them (the show: once the slide's transition is done).
  void Start(double now, bool begin = true);
  void BeginAuto(double now);
  // Ends the running steps at once, at their end poses.
  void Finish();

  // The main sequence's next step, or false when there is none left (the slide is done). A click
  // while a step runs finishes it first, as in PowerPoint.
  bool Click(double now);
  // A click on a trigger shape: its sequence's next step. False if it has none (left).
  bool Trigger(uint32_t spid, double now);
  // Back one step: the poses before the last click. False at the slide's start.
  bool Back();
  // Every main step done, as when coming back to a slide from the next one.
  void FinishAll(double now);

  // Moves the running steps to `now`: steps that start after the previous one begin here.
  void Update(double now);
  bool Running(double now) const;
  // The main steps left to click through.
  bool HasNextStep() const;
  int StepIndex() const { return mainNext_; }

  Pose PoseOf(const TargetKey& key) const;
  // The pose a target's own animations give it, without its shape's (for paragraphs).
  const std::vector<std::pair<TargetKey, Pose>>& Poses() const { return poses_; }
  // Targets that move in the running steps (whose sprites must be redrawn each frame).
  const std::vector<TargetKey>& Moving() const { return moving_; }

 private:
  struct Run {
    const Step* step = nullptr;
    double start = 0;
  };
  Pose* Find(const TargetKey& key);
  void Apply(const Behavior& b, double local, bool settle);
  void Settle(const Step& step);
  void Recompute(double now);
  void StartStep(const Step* step, double now);

  const Timing* timing_ = nullptr;
  BoxFn box_ = nullptr;
  void* ctx_ = nullptr;
  std::vector<std::pair<TargetKey, Pose>> settled_;  // after every finished step
  std::vector<std::pair<TargetKey, Pose>> poses_;    // now
  std::vector<std::vector<std::pair<TargetKey, Pose>>> history_;  // settled_ before each main click
  std::vector<int> historyNext_;
  std::vector<Run> runs_;
  std::vector<TargetKey> moving_;
  int mainNext_ = 0;
  std::vector<int> triggerNext_;
};

}  // namespace fw::slide::show
