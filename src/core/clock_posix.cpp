/**
 * @file        core/clock_posix.cpp
 * @brief       POSIX platform clock implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC || REX_PLATFORM_SWITCH,
              "This file is POSIX-only");
// Horizon is not POSIX, but newlib supplies everything this file actually
// touches, so devkitA64 builds it unchanged rather than needing a twin.

#include <sys/time.h>

#include <cerrno>

#include <rex/logging.h>

namespace rex::chrono {

#if REX_PLATFORM_MAC || REX_PLATFORM_SWITCH
// libnx implements exactly two clock ids, CLOCK_REALTIME (1) and
// CLOCK_MONOTONIC (4); its clock_gettime rejects everything else with EINVAL.
// CLOCK_MONOTONIC_RAW is 5, so asking for it fails every single time - and
// because the assert below compiles out in release, the failure was silent and
// the timestamp was whatever the uninitialised timespec happened to hold.
// Every monotonic time in the runtime was garbage, which stopped the vblank
// pacer from ever deciding a frame interval had elapsed.
constexpr clockid_t kHostClock = CLOCK_MONOTONIC;
#else
constexpr clockid_t kHostClock = CLOCK_MONOTONIC_RAW;
#endif

uint64_t Clock::host_tick_frequency_platform() {
  timespec res;
  int error = clock_getres(kHostClock, &res);
  assert_zero(error);
  assert_zero(res.tv_sec);  // Sub second resolution is required.
  assert_true(res.tv_nsec > 0);

  // host_tick_count_platform returns elapsed nanoseconds, not ticks in units of
  // clock_getres. Report the frequency for those returned units.
  return 1000000000ull;
}

uint64_t Clock::host_tick_count_platform() {
  // Zeroed, and the result checked: a failing clock_gettime leaves this
  // untouched, and reading an uninitialised timespec turns a clock error into
  // arbitrary time travel rather than an obvious stop. The assert is a
  // debug-only aid and does not run in the builds that ship.
  timespec tp = {};
  int error = clock_gettime(kHostClock, &tp);
  assert_zero(error);
  if (error != 0) {
    // A clock that cannot be read is not something to paper over with a
    // plausible-looking number: everything paced by it would misbehave in ways
    // that look like anything but a broken clock.
    static bool reported = false;
    if (!reported) {
      reported = true;
      REXSYS_ERROR("clock_gettime({}) failed with errno {}; monotonic time is unavailable",
                   int(kHostClock), errno);
    }
    return 0;
  }

  return uint64_t(tp.tv_nsec) + uint64_t(tp.tv_sec) * 1000000000ull;
}

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
