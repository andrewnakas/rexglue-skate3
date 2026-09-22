/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 *
 * See include/rex/system/guest_pause.h for why this is cooperative.
 */

#include <rex/system/guest_pause.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>

#include <rex/logging.h>

namespace rex {
namespace system {

namespace {

std::mutex g_mutex;
// Woken when a pause is released.
std::condition_variable g_release_cv;
// Woken as each thread parks, so the requester can stop waiting the moment the
// last one arrives instead of always spending its whole budget.
std::condition_variable g_parked_cv;

uint32_t g_parked_count = 0;

// The lifecycle callback runs on a real thread, and on iOS that thread is
// inside a UIKit delegate. Parking it would hang the app in its own suspend
// handler with nothing left running to release it.
thread_local bool t_excluded = false;

}  // namespace

namespace internal {

std::atomic<bool> guest_pause_requested_{false};

void GuestPausePark() {
  if (t_excluded) {
    return;
  }
  std::unique_lock<std::mutex> lock(g_mutex);
  // Re-check under the lock: the pause may have been released between the
  // relaxed load in Checkpoint() and getting here, and parking then would wait
  // for a wake-up that has already happened.
  if (!guest_pause_requested_.load(std::memory_order_relaxed)) {
    return;
  }
  ++g_parked_count;
  g_parked_cv.notify_all();
  g_release_cv.wait(lock, [] { return !guest_pause_requested_.load(std::memory_order_relaxed); });
  --g_parked_count;
}

}  // namespace internal

uint32_t RequestGuestPause(std::chrono::milliseconds budget) {
  internal::guest_pause_requested_.store(true, std::memory_order_relaxed);

  // Threads trickle in rather than arriving together, and there is no reliable
  // count of how many are expected - the title spawns its own. So wait for the
  // arrivals to go quiet rather than for a target: whenever nothing new has
  // parked for kQuietPeriod, everything that was going to reach a checkpoint
  // in time has. The budget is the hard ceiling either way.
  constexpr auto kQuietPeriod = std::chrono::milliseconds(40);
  std::unique_lock<std::mutex> lock(g_mutex);
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    const uint32_t before = g_parked_count;
    const auto quiet_until =
        std::min(deadline, std::chrono::steady_clock::now() + kQuietPeriod);
    g_parked_cv.wait_until(lock, quiet_until);
    if (g_parked_count == before) {
      break;
    }
  }
  return g_parked_count;
}

void ReleaseGuestPause() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!internal::guest_pause_requested_.load(std::memory_order_relaxed)) {
      return;
    }
    internal::guest_pause_requested_.store(false, std::memory_order_relaxed);
  }
  g_release_cv.notify_all();
}

uint32_t ParkedGuestThreadCount() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_parked_count;
}

void ExcludeCurrentThreadFromGuestPause() {
  t_excluded = true;
}

}  // namespace system
}  // namespace rex
