/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 *
 * Cooperative parking for guest threads across a suspend.
 *
 * The problem this solves is platform-specific and measured: on Android the PPC
 * guest threads keep running at full speed while the app is backgrounded. SDL
 * blocks only its own UI thread (Android_PumpEvents waits with an infinite
 * timeout while paused), so six pinned guest threads carry on burning battery
 * and, worse, keep the process looking busy and expensive to a system deciding
 * what to reclaim. On iOS they run until the kernel suspends the whole process
 * a moment later, which is better but still not nothing.
 *
 * Why cooperative rather than XThread::Suspend: suspending a thread at an
 * arbitrary instruction can catch it owning a host lock - the allocator's, most
 * dangerously - and anything that then allocates deadlocks. KernelState::
 * TerminateTitle documents exactly that hazard, and its REX_PLATFORM_MAC branch
 * refuses to suspend at all because of it. Parking threads at points they have
 * chosen to reach cannot deadlock: a parked thread holds no host lock, and a
 * guest thread blocked on a guest object held by a parked thread simply reaches
 * its own checkpoint and parks too.
 *
 * The bound is on the REQUESTER, not the parked thread. RequestPause waits only
 * briefly for threads to arrive; whatever has not parked keeps running exactly
 * as it does today. This matters because iOS allows roughly five seconds to go
 * quiescent before killing the app without writing a crash report, and blocking
 * the lifecycle callback waiting for a thread deep in a compute loop would
 * spend that budget to no purpose.
 */

#ifndef REX_SYSTEM_GUEST_PAUSE_H_
#define REX_SYSTEM_GUEST_PAUSE_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex {
namespace system {

namespace internal {
// Exposed only so Checkpoint() can inline its fast path. Do not read directly.
extern std::atomic<bool> guest_pause_requested_;
// Parks the calling thread. Never call this directly; call Checkpoint().
void GuestPausePark();
}  // namespace internal

// Asks guest threads to park at their next safe point, then waits up to
// `budget` for them to arrive. Returns the number parked when it gave up
// waiting - informational only, and never a reason to refuse to suspend.
//
// Safe to call twice; the second call is a no-op that still reports the count.
uint32_t RequestGuestPause(std::chrono::milliseconds budget);

// Releases every parked thread. Idempotent, and safe to call when no pause was
// ever requested - which matters because the foreground events that trigger it
// are themselves unreliable (WILL_ENTER_FOREGROUND is delivered for only about
// half of the resumes measured on device, so both foreground events call this).
void ReleaseGuestPause();

// How many guest threads are parked right now.
uint32_t ParkedGuestThreadCount();

// Marks the calling thread as one that must never park - the thread servicing
// the lifecycle callback, above all, since parking it would hang the app inside
// the suspend handler with nothing left running to release it.
void ExcludeCurrentThreadFromGuestPause();

// Called from guest-thread safe points: kernel waits and the frame boundary.
// One relaxed atomic load when no pause is pending, which is why it can sit in
// the hottest call in the title.
inline void GuestPauseCheckpoint() {
  if (internal::guest_pause_requested_.load(std::memory_order_relaxed)) {
    internal::GuestPausePark();
  }
}

}  // namespace system
}  // namespace rex

#endif  // REX_SYSTEM_GUEST_PAUSE_H_
