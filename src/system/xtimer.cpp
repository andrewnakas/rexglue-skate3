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

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <atomic>

#include <rex/system/xtimer.h>

namespace rex::system {

XTimer::XTimer(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() = default;

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = rex::thread::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = rex::thread::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

extern "C" {
// How far ahead the last timer was armed, and the furthest ever seen. A guest
// thread waiting on a timer that never fires is indistinguishable from one
// waiting on a timer armed for next year.
std::atomic<int64_t> rex_diag_timer_last_due_ms{0};
std::atomic<int64_t> rex_diag_timer_max_due_ms{0};
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                          uint32_t routine_arg, bool resume) {
  using rex::chrono::WinSystemClock;
  using rex::chrono::XSystemClock;
  // Caller is checking for STATUS_TIMER_RESUME_IGNORED.
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  period_ms = chrono::Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = rex::chrono::hundrednanoseconds(-due_time);
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::from_file_time(due_time));
  }

  // A due time already in the past means "signal immediately", and titles do
  // ask for exactly that: Skate 3 arms this timer with an absolute time at or
  // near the file-time epoch, which lands over four centuries back.
  //
  // Left alone it does not fire early, it never fires at all. The timer queue
  // works in steady_clock, whose epoch is boot; converting a 17th-century
  // instant into it needs about 1.3e19 nanoseconds, which does not fit in the
  // signed 64-bit representation and wraps to a point in the far future. The
  // timer is then queued for a date it will never reach, the guest thread
  // waiting on it sleeps for ever, and on this title that thread is the one
  // that drives the front end - so the game boots, renders, plays audio and
  // simply never starts.
  //
  // Clamping here, before the conversion, keeps the arithmetic in range and
  // gives the guest the immediate signal it asked for.
  {
    const auto now_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now());
    if (due_tp < now_tp) {
      due_tp = now_tp;
    }
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    callback = [this]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = rex::chrono::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      REXSYS_INFO("XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
                  callback_routine_, callback_routine_arg_, time_low, time_high);
      callback_thread_->EnqueueApc(callback_routine_, callback_routine_arg_, time_low, time_high);
    };
  }

  // How far out the timer is actually being armed. Every guest thread in this
  // port is parked on an Event, a Semaphore or a Timer with nothing signalling
  // them, and a due time computed from a clock that does not behave would arm
  // timers so far ahead that they never fire - which looks exactly like this.
  {
    const auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              due_tp - std::chrono::clock_cast<WinSystemClock>(XSystemClock::now()))
                              .count();
    static std::atomic<uint64_t> armed{0};
    const uint64_t n = armed.fetch_add(1, std::memory_order_relaxed) + 1;
    // Every 500th is a sample, and a sample is no use when the arm that matters
    // is the last one before everything stops. Report anything unusual too: an
    // absolute due time that converted badly would land far in the future, sit
    // in the queue and never fire, which is exactly the shape of this stall.
    const bool unusual = delta_ms > 1000 || delta_ms < 0;
    if (n <= 8 || (n % 500) == 0 || unusual) {
      REXSYS_WARN("[timer] arm #{}: due in {} ms, period {} ms{}", n, delta_ms, period_ms,
                  unusual ? "   <- UNUSUAL" : "");
    }
    rex_diag_timer_last_due_ms.store(int64_t(delta_ms), std::memory_order_relaxed);
    if (delta_ms > rex_diag_timer_max_due_ms.load(std::memory_order_relaxed)) {
      rex_diag_timer_max_due_ms.store(int64_t(delta_ms), std::memory_order_relaxed);
    }
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result =
        timer_->SetRepeatingAt(due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::system
