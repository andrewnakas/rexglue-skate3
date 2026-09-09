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

#include <algorithm>
#include <forward_list>

#include <disruptorplus/blocking_wait_strategy.hpp>
#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>

#include <rex/assert.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

namespace dp = disruptorplus;

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

extern "C" {
std::atomic<uint64_t> rex_diag_tq_dropped{0};
std::atomic<uint32_t> rex_diag_tq_drop_state{0};
std::atomic<uint64_t> rex_diag_disarm_blocked{0};
std::atomic<uint64_t> rex_diag_disarm_woke{0};
}

namespace {
std::atomic<uint64_t> g_tq_iterations{0};
std::atomic<uint64_t> g_tq_dispatched{0};
std::atomic<uint64_t> g_tq_completed{0};
std::atomic<uint64_t> g_tq_queued{0};
std::atomic<uint64_t> g_tq_claim_waits{0};
std::atomic<uint64_t> g_tq_pending{0};
}  // namespace

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ =
        std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");

    while (!stop_token.stop_requested()) {
      g_tq_iterations.fetch_add(1, std::memory_order_relaxed);
      {
        // Consume new wait items and add them to sorted wait queue
        dp::sequence_t available = claim_strategy_.wait_until_published(
            next_sequence, next_sequence - 1,
            wait_queue_.empty() ? clock::time_point::max() : wait_queue_.front()->due_);

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            g_tq_dispatched.fetch_add(1, std::memory_order_relaxed);
            wait_item->callback_(wait_item->userdata_);
            g_tq_completed.fetch_add(1, std::memory_order_relaxed);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              // Never schedule a recurring timer in the past. Advancing purely
              // by the interval means a dispatch that ran late stays late, and
              // the next due time is already behind - so the item is due again
              // the instant it is re-queued, and the loop dispatches it flat
              // out trying to catch up on time that cannot be recovered.
              //
              // Measured on this console: two recurring timers between them
              // drove the dispatch thread to a thousand callbacks a second and
              // it never slept. On three cores that is most of one core spent
              // achieving nothing, and it leaves the queue permanently at its
              // deadline, which is where a one-shot timer went missing and took
              // the title's timer thread down with it. QueueTimer already
              // clamps this way when an item is first introduced; the recurring
              // path was the one place that did not.
              const auto now = clock::now();
              if (wait_item->due_ < now) {
                // One interval ahead of now, not now: clamping to now leaves
                // the item due again the instant the loop looks at it, which
                // is the same storm by another route.
                wait_item->due_ = now + wait_item->interval_;
              }
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // The item was not idle, so its callback is skipped and the item is
            // discarded. In release the assert below compiles out, so this is a
            // silent drop - and a dropped one-shot is a guest thread that waits
            // for ever. Counted, with the state that caused it.
            ++rex_diag_tq_dropped;
            rex_diag_tq_drop_state.store(uint32_t(state), std::memory_order_relaxed);
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
        g_tq_pending.store(uint64_t(std::distance(wait_queue_.begin(), wait_queue_.end())),
                           std::memory_order_relaxed);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    // claim_one() blocks when the introduction ring is full, which happens
    // whenever the dispatch thread stops consuming. Timing it separates
    // "nothing is arming timers" from "arming a timer is itself stuck" - the
    // caller is a guest thread, so a block here freezes guest code.
    g_tq_queued.fetch_add(1, std::memory_order_relaxed);
    const auto claim_begin = clock::now();
    auto sequence = claim_strategy_.claim_one();
    if (clock::now() - claim_begin > std::chrono::milliseconds(1)) {
      g_tq_claim_waits.fetch_add(1, std::memory_order_relaxed);
    }
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  // Blocking (condition-variable) waits, not the spin strategy: the dispatch
  // thread waits with a deadline of the next timer due time, and with a spin
  // strategy that wait yield-spins for the whole interval, permanently
  // burning most of a core while any guest timer is pending. Guest timers
  // need millisecond-class dispatch precision, which a condition variable
  // delivers comfortably.
  dp::blocking_wait_strategy wait_strategy_;
  dp::multi_threaded_claim_strategy<dp::blocking_wait_strategy> claim_strategy_;
  dp::sequence_barrier<dp::blocking_wait_strategy> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify.
      // This is std::atomic::wait, which on Horizon has no futex under it. If
      // the platform fallback ever fails to wake, the thread calling Disarm -
      // a GUEST thread, arming its next timer - blocks here for good, so the
      // slow path is counted.
      ++rex_diag_disarm_blocked;
      state_.wait(state, std::memory_order_acquire);
      ++rex_diag_disarm_woke;
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::move_only_function<void(void*)> callback,
                                       void* userdata, WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::move_only_function<void(void*)> callback,
                                            void* userdata, WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

TimerQueueDiagnostics GetTimerQueueDiagnostics() {
  const uint64_t dispatched = g_tq_dispatched.load(std::memory_order_relaxed);
  const uint64_t completed = g_tq_completed.load(std::memory_order_relaxed);
  return TimerQueueDiagnostics{g_tq_iterations.load(std::memory_order_relaxed),
                               dispatched,
                               completed,
                               g_tq_queued.load(std::memory_order_relaxed),
                               g_tq_claim_waits.load(std::memory_order_relaxed),
                               g_tq_pending.load(std::memory_order_relaxed),
                               dispatched != completed};
}

}  // namespace rex::thread
