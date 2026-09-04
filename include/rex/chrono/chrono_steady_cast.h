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

#include <rex/chrono/chrono.h>

// This is in a separate header because casting to and from steady time points
// usually doesn't make sense and is imprecise. However, NT uses the FileTime
// epoch as a steady clock in waits. In such cases, include this header and use
// clock_cast<>().

namespace std::chrono {

// This conveniently works only for Host time domain because Guest needs
// additional scaling. Convert XSystemClock to WinSystemClock first if
// necessary.
template <>
struct clock_time_conversion<::rex::chrono::WinSystemClock, std::chrono::steady_clock> {
  // using NtSystemClock_ = ::rex::chrono::internal::NtSystemClock<domain_>;
  using WinSystemClock_ = ::rex::chrono::WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  typename WinSystemClock_::time_point operator()(
      const std::chrono::time_point<steady_clock_, Duration>& t) const {
    // Since there is no known epoch for steady_clock and even if, since it can
    // progress differently than other common clocks (e.g. stopping when the
    // computer is suspended), we need to use now() which introduces
    // imprecision.
    // Memory fences to keep the clock fetches close together to
    // minimize drift. This pattern was benchmarked to give the lowest
    // conversion error: error = sty_tpoint -
    // clock_cast<sty>(clock_cast<nt>(sty_tpoint));
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = std::chrono::floor<WinSystemClock_::duration>(t - steady_now);
    return nt_now + delta;
  }
};

template <>
struct clock_time_conversion<std::chrono::steady_clock, ::rex::chrono::WinSystemClock> {
  using WinSystemClock_ = ::rex::chrono::WinSystemClock;
  using steady_clock_ = std::chrono::steady_clock;

  template <typename Duration>
  steady_clock_::time_point operator()(
      const std::chrono::time_point<WinSystemClock_, Duration>& t) const {
    std::atomic_thread_fence(std::memory_order_acq_rel);
    auto steady_now = steady_clock_::now();
    auto nt_now = WinSystemClock_::now();
    std::atomic_thread_fence(std::memory_order_acq_rel);

    auto delta = t - nt_now;
    // Clamp before widening. steady_clock counts NANOseconds while
    // WinSystemClock counts hundreds of them, so converting this delta
    // multiplies it by a hundred - and a delta that sits comfortably inside
    // int64 in 100 ns units can overflow it in nanoseconds. That is not
    // hypothetical: a guest timer set to an absolute time near the 1601 epoch
    // produces a delta of about minus four hundred and twenty-five years, which
    // wraps to a deadline roughly a hundred and sixty years in the FUTURE. The
    // timer is then queued and never fires, and every thread behind it waits
    // forever. Signed overflow is undefined behaviour, so the wrap is not even
    // guaranteed to be the same twice.
    //
    // A century either side is far outside anything a real timer asks for and
    // far inside the range where the multiply is safe.
    using SteadyDuration = steady_clock_::duration;
    constexpr auto kLimit = std::chrono::duration_cast<decltype(delta)>(
        std::chrono::duration<int64_t>(int64_t(100) * 365 * 24 * 60 * 60));
    if (delta > kLimit) {
      delta = kLimit;
    } else if (delta < -kLimit) {
      delta = -kLimit;
    }
    return steady_now + std::chrono::duration_cast<SteadyDuration>(delta);
  }
};

}  // namespace std::chrono
