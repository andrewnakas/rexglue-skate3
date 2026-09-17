/**
 * @file        core/seh_switch.cpp
 * @brief       Structured-exception shims for Horizon.
 *
 * The POSIX file turns SIGSEGV/SIGBUS/SIGFPE/SIGILL into a C++ throw from
 * inside the signal handler, so a guest fault can be caught and turned into a
 * guest-visible exception. Horizon has no signals, and its exception handler
 * runs on a separate stack with no way to resume the faulting thread or to
 * unwind through the fault - see exception_handler_switch.cpp, where a fault is
 * reported and the process ends.
 *
 * So the SEH region here is bookkeeping only: it tracks whether the current
 * thread is inside one, which the guest code checks, and never fires. A fault
 * that would have been an SehException on POSIX is fatal on this platform, and
 * the crash report says where.
 */

#include <rex/platform.h>
#include <rex/platform/seh.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

#include <cstdlib>

namespace rex::platform {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(u32 /*code*/, void* /*exception_pointers*/) {
  // Windows-only concept; the POSIX file returns 0 here for the same reason.
  return 0;
}

[[noreturn]] void seh_rethrow() {
  // Nothing was ever caught, so there is nothing to re-raise. Reaching here
  // means a caller believed it had captured a fault it could not have.
  std::abort();
}

void seh_initialize() {
  // The exception handler is installed once by main_switch.cpp via libnx's
  // __libnx_exception_handler hook, not per SEH region.
  g_seh_initialized.store(true, std::memory_order_relaxed);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform
