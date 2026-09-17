/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

#include <rex/std_compat.h>

// This is a platform independent implementation of a timer queue similar to
// Windows CreateTimerQueueTimer with WT_EXECUTEINTIMERTHREAD.

namespace rex::thread {

class TimerQueue;

struct TimerQueueWaitItem {
  using clock = std::chrono::steady_clock;

  TimerQueueWaitItem(std::move_only_function<void(void*)> callback, void* userdata,
                     TimerQueue* parent_queue, clock::time_point due, clock::duration interval)
      : callback_(std::move(callback)),
        userdata_(userdata),
        parent_queue_(parent_queue),
        due_(due),
        interval_(interval),
        state_(State::kIdle) {}

  // Cancel the pending wait item. No callbacks will be running after this call.
  // The function blocks if a callback is running and returns only after the
  // callback has finished (except when called from the corresponding callback
  // itself, where it will mark the wait item for disarmament and return
  // immediately). Deadlocks are possible when a lock is held during disamament
  // and the corresponding callback is running concurrently, trying to acquire
  // said lock.
  void Disarm();

  friend TimerQueue;

 private:
  enum class State : uint_least8_t {
    kIdle = 0,                // Waiting for the due time
    kInCallback,              // Callback is being executed
    kInCallbackSelfDisarmed,  // Callback is being executed and disarmed itself
    kDisarmed                 // Disarmed, waiting for destruction
  };
  static_assert(std::atomic<State>::is_always_lock_free);

  std::move_only_function<void(void*)> callback_;
  void* userdata_;
  TimerQueue* parent_queue_;
  clock::time_point due_;
  clock::duration interval_;  // zero if not recurring
  std::atomic<State> state_;
};

std::weak_ptr<TimerQueueWaitItem> QueueTimerOnce(std::move_only_function<void(void*)> callback,
                                                 void* userdata,
                                                 TimerQueueWaitItem::clock::time_point due);

// Callback is first executed at due, then again repeatedly after interval
// passes (unless interval == 0). The first callback will be scheduled at
// `max(now() - interval, due)` to mitigate callback flooding.
std::weak_ptr<TimerQueueWaitItem> QueueTimerRecurring(std::move_only_function<void(void*)> callback,
                                                      void* userdata,
                                                      TimerQueueWaitItem::clock::time_point due,
                                                      TimerQueueWaitItem::clock::duration interval);

// A window into the dispatch thread, because a timer that stops arriving is
// indistinguishable from one that was never set. Two failures have to be told
// apart: the dispatch thread stuck inside a guest callback, which stops every
// timer at once, and the introduction ring filling up, which blocks whoever
// tries to arm the next one.
struct TimerQueueDiagnostics {
  uint64_t iterations;   // trips round the dispatch loop
  uint64_t dispatched;   // callbacks entered
  uint64_t completed;    // callbacks returned
  uint64_t queued;       // timers introduced
  uint64_t claim_waits;  // arms that had to wait for ring space
  uint64_t pending;      // timers currently in the sorted queue
  bool in_callback;
};
TimerQueueDiagnostics GetTimerQueueDiagnostics();

}  // namespace rex::thread
