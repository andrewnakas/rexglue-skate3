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

#include <atomic>

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
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
    // An absolute guest time, which is allowed to be in the PAST - and after a
    // suspend it reliably is.
    //
    // The guest computes these deadlines by subtraction. Across a suspend the
    // subtraction goes negative and the sign flips, so what was meant as "16 ms
    // from now" (a negative, relative value) arrives as a small POSITIVE one:
    // an absolute time a few milliseconds after the 1601 epoch. On hardware
    // that simply fires immediately, which is why the game is written this way
    // and never notices.
    //
    // Here it was fatal. The conversion below subtracts ~425 years, and while
    // that fits comfortably in the 100 ns units of the guest clock, converting
    // it to the nanoseconds of steady_clock multiplies it by a hundred and
    // overflows int64 - wrapping a deadline in the deep past into one about a
    // hundred and sixty years in the FUTURE. The timer is queued and never
    // fires; the guest thread waiting on it waits forever; the game's main
    // thread waits on that thread, and every other guest thread waits on the
    // main thread. The renderer is not in that chain, so it carries on drawing
    // a world that can no longer change - which is exactly what the Steam Deck
    // wake-from-suspend freeze looks like, and why it took twenty-three builds
    // to find: nothing fails, nothing errors, and every layer looks healthy.
    //
    // Clamping to "now" restores the hardware behaviour and removes the
    // overflow in one go.
    const auto requested = XSystemClock::from_file_time(due_time);
    const auto guest_now = XSystemClock::now();
    if (requested <= guest_now) {
      // Rate-limited. This is normal-but-notable: the guest computes these
      // deadlines by subtraction and legitimately lands in the past now and
      // then, so it must not be able to flood a log, but the first few are
      // worth having because they are the fingerprint of the suspend/resume
      // freeze this clamp exists to prevent.
      static std::atomic<uint64_t> s_clamped{0};
      static std::atomic<int64_t> s_last_log_ns{0};
      const uint64_t clamped = s_clamped.fetch_add(1, std::memory_order_relaxed) + 1;
      const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
      int64_t last_ns = s_last_log_ns.load(std::memory_order_relaxed);
      if (clamped <= 4 ||
          (now_ns - last_ns >= 1'000'000'000 &&
           s_last_log_ns.compare_exchange_strong(last_ns, now_ns, std::memory_order_relaxed))) {
        const auto late_by = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 guest_now - requested)
                                 .count();
        REXSYS_WARN(
            "XTimer: absolute due time {} is {} ms in the past - firing immediately, as the "
            "hardware would ({} clamped so far). Left unclamped this overflows into a deadline "
            "far in the future and the timer never fires at all.",
            due_time, late_by, clamped);
      }
      due_tp = std::chrono::clock_cast<WinSystemClock>(guest_now);
    } else {
      due_tp = std::chrono::clock_cast<WinSystemClock>(requested);
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
