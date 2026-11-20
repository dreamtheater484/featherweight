#include "sheet/calc/Workers.h"

#include <windows.h>

#include <algorithm>

namespace fw::sheet::calc {

struct Workers::State {
  SRWLOCK lock = SRWLOCK_INIT;
  CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT, done = CONDITION_VARIABLE_INIT;
  uint64_t job = 0;  // bumped for each Run
  bool quit = false;
  int busy = 0;      // workers still in the current job
  size_t n = 0, grain = 1;
  std::atomic<size_t> next{0};
  const std::function<void(int, size_t, size_t)>* fn = nullptr;
};

namespace {
struct Start {
  Workers* pool;
  int worker;
};
}  // namespace

Workers::Workers(int threads) : count_(std::max(1, threads)), s_(new State) {
  for (int w = 1; w < count_; ++w) {
    auto* st = new Start{this, w};
    HANDLE h = CreateThread(nullptr, 256 * 1024, &Workers::Main, st, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {
      delete st;
      count_ = w;
      break;
    }
    threads_.push_back(h);
  }
}

Workers::~Workers() {
  AcquireSRWLockExclusive(&s_->lock);
  s_->quit = true;
  ReleaseSRWLockExclusive(&s_->lock);
  WakeAllConditionVariable(&s_->wake);
  for (void* h : threads_) {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
  }
  delete s_;
}

unsigned long __stdcall Workers::Main(void* arg) {
  Start st = *static_cast<Start*>(arg);
  delete static_cast<Start*>(arg);
  st.pool->Work(st.worker);
  return 0;
}

void Workers::Work(int worker) {
  uint64_t seen = 0;
  for (;;) {
    AcquireSRWLockExclusive(&s_->lock);
    while (!s_->quit && s_->job == seen) SleepConditionVariableSRW(&s_->wake, &s_->lock, INFINITE, 0);
    if (s_->quit) {
      ReleaseSRWLockExclusive(&s_->lock);
      return;
    }
    seen = s_->job;
    const auto* fn = s_->fn;
    const size_t n = s_->n, grain = s_->grain;
    ReleaseSRWLockExclusive(&s_->lock);
    for (size_t b; (b = s_->next.fetch_add(grain)) < n;) (*fn)(worker, b, std::min(n, b + grain));
    AcquireSRWLockExclusive(&s_->lock);
    if (--s_->busy == 0) WakeConditionVariable(&s_->done);
    ReleaseSRWLockExclusive(&s_->lock);
  }
}

void Workers::Run(size_t n, size_t grain, const std::function<void(int, size_t, size_t)>& fn) {
  if (n == 0) return;
  grain = std::max<size_t>(1, grain);
  if (count_ == 1 || n <= grain) {
    for (size_t b = 0; b < n; b += grain) fn(0, b, std::min(n, b + grain));
    return;
  }
  AcquireSRWLockExclusive(&s_->lock);
  s_->fn = &fn;
  s_->n = n;
  s_->grain = grain;
  s_->next.store(0);
  s_->busy = count_ - 1;
  ++s_->job;
  ReleaseSRWLockExclusive(&s_->lock);
  WakeAllConditionVariable(&s_->wake);
  for (size_t b; (b = s_->next.fetch_add(grain)) < n;) fn(0, b, std::min(n, b + grain));
  AcquireSRWLockExclusive(&s_->lock);
  while (s_->busy > 0) SleepConditionVariableSRW(&s_->done, &s_->lock, INFINITE, 0);
  s_->fn = nullptr;
  ReleaseSRWLockExclusive(&s_->lock);
}

}  // namespace fw::sheet::calc
