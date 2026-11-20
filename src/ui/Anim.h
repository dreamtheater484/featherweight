#pragma once

namespace anim {

// Milliseconds from a monotonic clock.
double Now();
// Keeps the frame clock running until `untilMs`.
void KeepAlive(double untilMs);
// True while any animation needs more frames.
bool Running();

// A window that draws its own frames at the display's refresh rate (the slide show). While it
// animates, the message loop gives it every frame instead of waiting for messages; Frame() waits
// for the display itself. Idle, it draws nothing and nothing runs.
class FrameSource {
 public:
  virtual ~FrameSource() = default;
  virtual bool Animating() const = 0;
  virtual void Frame() = 0;
};
void SetFrameSource(FrameSource* source);  // null: none
FrameSource* ActiveFrameSource();

}  // namespace anim

// A time-based eased value. Reading V() never needs ticking; the app keeps repainting while anim::Running().
class Anim {
 public:
  explicit Anim(float v = 0) : from_(v), to_(v) {}

  void To(float target, float durationMs = 120);
  void Snap(float v);
  float V() const;
  float Target() const { return to_; }

 private:
  float from_, to_;
  double start_ = 0;
  float duration_ = 0;
};
