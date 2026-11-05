#pragma once

#include <chrono>

namespace fw {

// When each stage of opening a document was reached, for the opening budgets (plan §11). Whoever
// opens a document calls Start() when the user asks for it, and the engines call Mark() as they
// pass each stage. `fwtool bench` reads the marks (tests/office/README.md), and so can the app.
class OpenTimings {
 public:
  enum Stage {
    Read,       // the file's bytes are in memory (the private snapshot)
    Package,    // the zip, content types and relationships are read
    Model,      // the document model is built: styles, numbering, the stories
    FirstPage,  // the first page is laid out
    Shown,      // the first page is on screen (the app only)
    Paginated,  // every page is laid out, so the page count is known
    kStages
  };

  void Start() {
    start_ = Clock::now();
    for (double& ms : ms_) ms = -1;
  }
  // Only the first mark of a stage counts.
  void Mark(Stage stage) {
    if (ms_[stage] < 0) ms_[stage] = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
  }
  // Milliseconds from Start() to the stage, or -1 if it wasn't reached.
  double Ms(Stage stage) const { return ms_[stage]; }

  static const char* Name(Stage stage) {
    static const char* const kNames[] = {"read", "package", "model", "first page", "shown", "paginated"};
    return kNames[stage];
  }

 private:
  using Clock = std::chrono::steady_clock;
  Clock::time_point start_ = Clock::now();
  double ms_[kStages] = {-1, -1, -1, -1, -1, -1};
};

}  // namespace fw
