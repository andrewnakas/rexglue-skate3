/*
 * Bigger default stacks for std::thread on Horizon.
 *
 * devkitA64's pthread_attr_init zeroes the attribute, and libnx turns a zero
 * stack size into 128 KB. Every std::thread in the engine therefore runs on a
 * 128 KB stack, against the 8 MB it gets on Linux and Android and the 512 KB it
 * gets on iOS. This code is Xenia-derived and was written against those
 * budgets: the guest recompiler, the XMA decoder and the renderer all have deep
 * call chains and large frames.
 *
 * Horizon does not put a guard page below a thread stack, so overflowing one
 * does not fault at the point of overflow. It quietly writes over whatever is
 * mapped below, and the thread dies later at a nonsense address - usually just
 * after returning from a call, because the saved return address is what got
 * overwritten. That is a very expensive bug to find from the crash alone, so
 * the default is raised here rather than left to be rediscovered.
 *
 * Wrapping rather than redefining: pthread_create lives in libc, and defining a
 * second one would be a duplicate symbol. -Wl,--wrap sends callers here and
 * leaves the original reachable as __real_pthread_create.
 */

#include <pthread.h>
#include <cstddef>

namespace {

// 1 MB, matching what threading_switch.cpp gives the engine's own threads.
// Must be a multiple of the page size: libnx rejects a misaligned stack size.
constexpr size_t kMinimumStackSize = 0x100000;

}  // namespace

extern "C" {

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg) {
  pthread_attr_t local;
  if (attr) {
    size_t requested = 0;
    if (pthread_attr_getstacksize(attr, &requested) == 0 &&
        requested >= kMinimumStackSize) {
      // The caller asked for at least as much as we would impose; respect it.
      return __real_pthread_create(thread, attr, start_routine, arg);
    }
    local = *attr;
  } else if (pthread_attr_init(&local) != 0) {
    return __real_pthread_create(thread, attr, start_routine, arg);
  }

  if (pthread_attr_setstacksize(&local, kMinimumStackSize) != 0) {
    // Leave the caller's request alone rather than fail the thread outright.
    return __real_pthread_create(thread, attr, start_routine, arg);
  }

  const int rc = __real_pthread_create(thread, &local, start_routine, arg);
  if (rc == 0) {
    return 0;
  }
  // A bigger stack is an improvement, not a requirement. If the larger request
  // cannot be satisfied - most likely because memory is tight - fall back to
  // what the caller originally asked for rather than fail the thread and turn
  // a tuning change into a crash.
  return __real_pthread_create(thread, attr, start_routine, arg);
}

}  // extern "C"
