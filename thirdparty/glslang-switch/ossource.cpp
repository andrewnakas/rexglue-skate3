//
// Horizon (Nintendo Switch) backend for glslang's OS layer.
//
// The Unix version of this file is written against pthreads and thread
// cancellation, neither of which libnx provides. Everything it is actually
// asked for - a handful of thread-local slots and one global lock - is in the
// C++ standard library, so this backend is written against that instead of
// against an OS at all.
//
// Added downstream for the Skate 3 Switch port; not part of upstream glslang.
//

#include "../osinclude.h"

#include <cstdint>
#include <mutex>

namespace glslang {

namespace {

// glslang allocates a very small number of these - one for its per-thread pool
// allocator, one for its thread-local index - so a fixed table is simpler and
// faster than anything dynamic, and it cannot fail at an awkward moment.
constexpr size_t kMaxTlsSlots = 8;

std::mutex& SlotMutex() {
  static std::mutex mutex;
  return mutex;
}

bool g_slot_used[kMaxTlsSlots] = {};
thread_local void* t_slot_value[kMaxTlsSlots] = {};

// An index is handed back as a pointer, and zero means invalid, so slots are
// numbered from one and converted at the boundary.
inline size_t IndexOf(OS_TLSIndex index) {
  return reinterpret_cast<uintptr_t>(index) - 1;
}

inline OS_TLSIndex ToIndex(size_t slot) {
  return reinterpret_cast<OS_TLSIndex>(static_cast<uintptr_t>(slot + 1));
}

}  // namespace

OS_TLSIndex OS_AllocTLSIndex() {
  std::lock_guard<std::mutex> guard(SlotMutex());
  for (size_t slot = 0; slot < kMaxTlsSlots; ++slot) {
    if (!g_slot_used[slot]) {
      g_slot_used[slot] = true;
      return ToIndex(slot);
    }
  }
  return OS_INVALID_TLS_INDEX;
}

bool OS_SetTLSValue(OS_TLSIndex nIndex, void* lpvValue) {
  const size_t slot = IndexOf(nIndex);
  if (nIndex == OS_INVALID_TLS_INDEX || slot >= kMaxTlsSlots) {
    return false;
  }
  t_slot_value[slot] = lpvValue;
  return true;
}

void* OS_GetTLSValue(OS_TLSIndex nIndex) {
  const size_t slot = IndexOf(nIndex);
  if (nIndex == OS_INVALID_TLS_INDEX || slot >= kMaxTlsSlots) {
    return nullptr;
  }
  return t_slot_value[slot];
}

bool OS_FreeTLSIndex(OS_TLSIndex nIndex) {
  const size_t slot = IndexOf(nIndex);
  if (nIndex == OS_INVALID_TLS_INDEX || slot >= kMaxTlsSlots) {
    return false;
  }
  std::lock_guard<std::mutex> guard(SlotMutex());
  g_slot_used[slot] = false;
  return true;
}

namespace {
std::mutex& GlobalLock() {
  static std::mutex mutex;
  return mutex;
}
}  // namespace

void InitGlobalLock() {}

void GetGlobalLock() { GlobalLock().lock(); }

void ReleaseGlobalLock() { GlobalLock().unlock(); }

// The Unix backend runs this from a pthread cancellation handler, so that a
// thread which is destroyed mid-compile still returns its pool. There is no
// cancellation here and nothing calls this, but glslang references it.
void OS_CleanupThreadData(void) {}

void OS_DumpMemoryCounters() {}

}  // namespace glslang
