#pragma once

// A small pool of worker threads for multi-threaded recalculation (S4, plan §7.3): one job at a
// time, split over an index range; the calling thread takes part. Win32 threads and locks only.

#include <atomic>
#include <cstddef>
#include <functional>
#include <vector>

namespace fw::sheet::calc {

class Workers {
 public:
  // `threads` in all, the caller included (so threads - 1 are started).
  explicit Workers(int threads);
  ~Workers();
  Workers(const Workers&) = delete;
  Workers& operator=(const Workers&) = delete;

  int Count() const { return count_; }
  // Runs fn(worker, begin, end) over [0, n) in pieces of at most `grain`, on every thread; worker 0
  // is the caller. Returns once every piece is done.
  void Run(size_t n, size_t grain, const std::function<void(int worker, size_t begin, size_t end)>& fn);

 private:
  struct State;
  static unsigned long __stdcall Main(void* arg);
  void Work(int worker);

  int count_ = 1;
  State* s_ = nullptr;
  std::vector<void*> threads_;
};

}  // namespace fw::sheet::calc
