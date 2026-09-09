/**
 * @file        core/threading_switch.cpp
 * @brief       Threading on Horizon.
 *
 * The synchronisation primitives here - events, semaphores, mutants, timers and
 * the multi-handle wait - are the POSIX ones unchanged. They are built on
 * std::mutex and std::condition_variable, which devkitA64's libstdc++ maps onto
 * libnx locks, and nothing in them is POSIX-specific once the robust-mutex path
 * (which Horizon has no equivalent for) is compiled out.
 *
 * What is genuinely different is thread creation and scheduling, and it matters
 * more here than on any other platform this engine runs on:
 *
 *   Three cores, not four. The fourth is the system's. An application that
 *   spreads itself over four gets nothing on the fourth and loses the work.
 *
 *   The scheduler is strictly priority-preemptive, with one exception: at
 *   priority 0x3B on cores 0-2 (0x3F on core 3) threads of equal priority are
 *   time-sliced. Everywhere else a thread runs until it blocks or yields. A
 *   guest thread that spins at a better priority than its siblings does not
 *   merely waste a core, it starves them - which on three cores is the whole
 *   frame. So everything that spins or carries guest work runs at 0x3B, and
 *   MaybeYield is a real syscall rather than a hint.
 *
 *   Placement is by name, not by count. The engine only asks for affinity when
 *   it sees six or more processors, which never happens here, so the map below
 *   assigns each known thread a core and a priority as it names itself. This is
 *   the same mechanism the Android port uses, for the same reason.
 *
 * There are no signals. Suspend and resume go through svcSetThreadActivity, and
 * a queued user callback is a flag the alertable waits already poll rather than
 * an interrupt.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

#include <switch.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <deque>
#include <limits>
#include <memory>
#include <string_view>

#include <unistd.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/string/util.h>
#include <rex/thread/timer_queue.h>

// Horizon has no robust mutexes, so the recovery path the Linux build uses when
// a lock's owner dies is compiled out, exactly as it is on macOS and Android.
#define REX_HAS_ROBUST_MUTEX 0

REXCVAR_DEFINE_STRING(
    switch_thread_placement_map, "", "Threading",
    "Per-thread core and priority on Switch, as "
    "\"prefix=core:<0-2|0+1|any>[,prio:<28-59>];prefix=...\" matched against the "
    "thread name (first matching prefix wins; empty disables the feature). "
    "Priority runs from 28 (most urgent) to 59, and 59 is special: it is the "
    "only level on cores 0-2 where threads of equal priority are time-sliced. "
    "Anything that spins - the command processor, the guest's own wait loops - "
    "must sit there, or it holds its core against every sibling until it "
    "blocks. Without a map the three cores are shared by whatever order the "
    "threads happened to start in.");

REXCVAR_DEFINE_BOOL(
    switch_yield_with_migration, true, "Threading",
    "Yield with core migration (svcSleepThread(-1)) rather than without "
    "(svcSleepThread(0)). Without migration a yield only offers the core to "
    "threads already queued on it, so a guest thread spinning on core 0 cannot "
    "hand its slice to the worker it is waiting for on core 2 - which is the "
    "shape of every job-manager wait in this title. With migration the kernel "
    "may pull a runnable thread across, which is what a three core machine "
    "needs.");

namespace {

// The system keeps core 3 for itself; an application is given 0, 1 and 2.
constexpr int kUsableCoreCount = 3;
constexpr u32 kAllCoresMask = (1u << kUsableCoreCount) - 1;

// libnx names these: 0x2C is the main thread's priority, 0x3B is the
// preemptive level on cores 0-2.
constexpr int kMainThreadPriority = 0x2C;
constexpr int kPreemptivePriority = 0x3B;
constexpr int kHighestAllowedPriority = 28;

bool ParseInt(std::string_view s, int& out) {
  return !s.empty() && std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc();
}

// What the map asks for, for a thread with this name. Parsing is separated from
// applying because two callers need the answer: the placement below, and
// set_priority, which must not let a caller undo the map minutes later.
struct Placement {
  bool matched = false;
  int core = -1;       // -1: leave the core mask alone
  bool any_core = false;
  u32 core_mask = 0;   // 0: not a "core:a+b" entry
  int priority = -1;   // -1: leave the priority alone
};

Placement LookUpPlacement(std::string_view name) {
  Placement out;
  const std::string& map = REXCVAR_GET(switch_thread_placement_map);
  if (map.empty() || name.empty()) {
    return out;
  }

  std::string_view rest(map);
  while (!rest.empty()) {
    const size_t entry_end = rest.find(';');
    std::string_view entry = rest.substr(0, entry_end);
    rest = entry_end == std::string_view::npos ? std::string_view() : rest.substr(entry_end + 1);
    if (entry.empty()) {
      continue;
    }

    const size_t eq = entry.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view prefix = entry.substr(0, eq);
    if (prefix.empty() || name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }

    out.matched = true;

    std::string_view fields = entry.substr(eq + 1);
    while (!fields.empty()) {
      const size_t field_end = fields.find(',');
      std::string_view field = fields.substr(0, field_end);
      fields = field_end == std::string_view::npos ? std::string_view() : fields.substr(field_end + 1);

      const size_t colon = field.find(':');
      if (colon == std::string_view::npos) {
        continue;
      }
      const std::string_view key = field.substr(0, colon);
      const std::string_view value = field.substr(colon + 1);
      if (key == "core") {
        if (value == "any") {
          out.any_core = true;
        } else if (value.find('+') != std::string_view::npos) {
          // A set of cores, "core:0+1". Between pinning to one core and
          // floating across all three there is a real middle: keep a thread off
          // the core doing the frame's serial work without nailing it down.
          // The emulated command processor is a whole frame's work on one
          // thread, and every floating thread that lands on its core is taken
          // straight out of the frame.
          u32 mask = 0;
          std::string_view rest_cores = value;
          while (!rest_cores.empty()) {
            const size_t plus = rest_cores.find('+');
            const std::string_view one = rest_cores.substr(0, plus);
            rest_cores = plus == std::string_view::npos ? std::string_view()
                                                        : rest_cores.substr(plus + 1);
            int parsed = 0;
            if (ParseInt(one, parsed) && parsed >= 0 && parsed < kUsableCoreCount) {
              mask |= 1u << parsed;
            }
          }
          if (mask != 0) {
            out.core_mask = mask;
          }
        } else {
          int parsed = 0;
          if (ParseInt(value, parsed) && parsed >= 0 && parsed < kUsableCoreCount) {
            out.core = parsed;
          }
        }
      } else if (key == "prio") {
        int parsed = 0;
        if (ParseInt(value, parsed) && parsed >= kHighestAllowedPriority &&
            parsed <= kPreemptivePriority) {
          out.priority = parsed;
        }
      }
    }

    return out;  // first matching prefix wins
  }
  return out;
}

// Apply the map to a thread by handle. svcSetThreadCoreMask and
// svcSetThreadPriority both take a handle and work on any thread of this
// process, so a thread named by its parent - which is most of them: the audio
// pump, the pipeline compilers, every guest thread the title does not name
// itself - can be placed too. Only threads that named themselves were being
// placed before, which left the rest at priority 59 on the process default
// core, all on top of each other.
void ApplyPlacementForThread(Handle handle, std::string_view name) {
  const Placement p = LookUpPlacement(name);
  if (!p.matched) {
    return;
  }
  if (p.any_core) {
    svcSetThreadCoreMask(handle, -1, kAllCoresMask);
  } else if (p.core_mask != 0) {
    // No ideal core: the mask is the whole instruction, and naming a preferred
    // core inside it would undo half the point.
    svcSetThreadCoreMask(handle, -1, p.core_mask & kAllCoresMask);
  } else if (p.core >= 0) {
    // Both the ideal core and the mask: the mask alone lets the scheduler
    // migrate, and pinning is the whole point for the few threads that ask.
    svcSetThreadCoreMask(handle, p.core, 1u << p.core);
  }
  if (p.priority >= 0) {
    svcSetThreadPriority(handle, p.priority);
  }
  // At warn, because the shipped log level is warn and a run that cannot show
  // where its threads went cannot explain its own frame rate.
  REXLOG_WARN("[thread] placed '{}' core={} mask=0x{:x} prio={}", name,
              p.any_core ? -1 : p.core, p.any_core ? kAllCoresMask : p.core_mask, p.priority);
}

void ApplyPlacementForCurrentThreadName(std::string_view name) {
  ApplyPlacementForThread(CUR_THREAD_HANDLE, name);
}

}  // namespace

namespace rex::thread {

void SwitchInitialize() {
  // The main thread arrives at hbloader's priority on whichever core it was
  // started on. Naming it later runs it through the placement map like any
  // other; nothing to latch here.
}

void SwitchShutdown() {}

template <typename _Rep, typename _Period>
inline timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

void EnableAffinityConfiguration() {}

uint32_t current_thread_system_id() {
  u64 thread_id = 0;
  if (R_FAILED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE))) {
    return 0;
  }
  return static_cast<uint32_t>(thread_id);
}

void MaybeYield() {
  // Zero means "yield to another thread on this core, without migrating"; -1
  // lets the kernel pull a runnable thread over from another core's queue.
  //
  // This is not advisory on Horizon the way sched_yield can be: outside the
  // preemptive priority it is the only thing that lets a sibling run at all.
  // With three cores and a title whose threads wait on each other across them,
  // the non-migrating form is nearly useless: a guest thread spinning on core 0
  // yields, finds nothing else queued on core 0, and carries straight on while
  // the worker it is waiting for is still queued behind someone on core 2.
  svcSleepThread(REXCVAR_GET(switch_yield_with_migration) ? -1 : 0);
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

void Sleep(std::chrono::microseconds duration) {
  svcSleepThread(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

thread_local bool alertable_state_ = false;
bool DispatchCurrentThreadUserCallback();
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    Sleep(std::min(remaining, std::chrono::microseconds(1000)));
  }
}

// Thread-local storage. newlib's pthread_key support is not something to rely
// on here, and a fixed table of slots indexed by handle is both simpler and
// faster: the engine allocates a handful of these once at startup and reads
// them on hot paths.
namespace {
constexpr size_t kMaxTlsSlots = 64;
std::atomic<uint64_t> tls_slots_in_use_{0};
thread_local uintptr_t tls_values_[kMaxTlsSlots] = {};
}  // namespace

TlsHandle AllocateTlsHandle() {
  uint64_t in_use = tls_slots_in_use_.load(std::memory_order_relaxed);
  for (;;) {
    size_t slot = kMaxTlsSlots;
    for (size_t i = 0; i < kMaxTlsSlots; ++i) {
      if (!(in_use & (1ull << i))) {
        slot = i;
        break;
      }
    }
    assert_true(slot < kMaxTlsSlots);
    if (slot >= kMaxTlsSlots) {
      return kInvalidTlsHandle;
    }
    if (tls_slots_in_use_.compare_exchange_weak(in_use, in_use | (1ull << slot),
                                                std::memory_order_acq_rel)) {
      return static_cast<TlsHandle>(slot);
    }
  }
}

bool FreeTlsHandle(TlsHandle handle) {
  if (handle >= kMaxTlsSlots) {
    return false;
  }
  tls_slots_in_use_.fetch_and(~(1ull << handle), std::memory_order_acq_rel);
  return true;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return handle < kMaxTlsSlots ? tls_values_[handle] : 0;
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  if (handle >= kMaxTlsSlots) {
    return false;
  }
  tls_values_[handle] = value;
  return true;
}


// Timer delivery counters. A timer that stops arriving looks the same from the
// outside whether the expiry was never dispatched or was dispatched and missed,
// and those have different fixes. Counted with C linkage so the one file that
// prints them needs no shared header.
extern "C" {
std::atomic<uint64_t> rex_diag_timer_setonce{0};
std::atomic<uint64_t> rex_diag_timer_setonce_armed{0};
std::atomic<uint64_t> rex_diag_timer_completion{0};
std::atomic<uint64_t> rex_diag_timer_signal{0};
std::atomic<uint64_t> rex_diag_timer_cancel{0};
}

class PosixConditionBase {
 public:
  PosixConditionBase() {
#if REX_HAS_ROBUST_MUTEX
    // Use robust mutexes so waits can recover if owner thread terminates.
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
      if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0) {
        auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
        pthread_mutex_destroy(native_mutex);
        pthread_mutex_init(native_mutex, &attr);
      }
      pthread_mutexattr_destroy(&attr);
    }
#endif
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    bool executed;
    auto predicate = [this] { return this->signaled(); };
#if REX_HAS_ROBUST_MUTEX
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
#else
    std::unique_lock<std::mutex> lock(mutex_);
#endif
    if (predicate()) {
      executed = true;
    } else {
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;  // Did not time out;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    if (executed) {
      post_execution();
      return WaitResult::kSuccess;
    } else {
      return WaitResult::kTimeout;
    }
  }

  // A multi-handle wait has no POSIX equivalent, so WaitMultiple scans the
  // handles itself - and with no way to be woken, it had to rescan on a timer.
  // That timer was 1ms, and the scan try-locks every handle: the audio worker
  // waits on ten of them and spent most of a performance core doing nothing
  // else.
  //
  // A shared condition gives the scan a way to be woken, so a rescan happens
  // because something was signalled rather than because a millisecond passed.
  // Two things keep that from being just as expensive:
  //
  // Only a handle somebody is actually multi-waiting on wakes it. The count
  // below is incremented for the duration of a scan; a signal to a handle with
  // no multi-waiters skips the notify entirely. Without it every event and
  // semaphore in the emulator - thousands a second - woke every multi-waiter,
  // the generation had always moved by the time a scan finished, the sleep was
  // therefore always skipped, and the "wait" was a spin. That is what the
  // profile showed: the audio worker alone was 53% of the process's cycles,
  // 93% of it inside this function.
  //
  // And a WaitAny locks one handle at a time instead of all of them at once. A
  // wait for *any* handle does not need a consistent snapshot across them: the
  // first signalled handle it finds is a correct answer. Locking them all did
  // need one, could not get it with try-lock under contention, and fell back to
  // yield-and-retry, which is the other way this function burned a core.
  // wait_all still takes every lock, because it does need the snapshot.
  //
  // Lock order is always handle mutex then this one, never the reverse: the
  // scan releases every handle lock before it waits here.
  static std::mutex& wait_any_mutex() {
    static std::mutex mutex;
    return mutex;
  }
  static std::condition_variable& wait_any_cond() {
    static std::condition_variable cond;
    return cond;
  }
  // Guarded by wait_any_mutex().
  static uint64_t& wait_any_generation() {
    static uint64_t generation = 0;
    return generation;
  }
  // Wakes every multi-handle scan. Used by the paths that are not a handle
  // signal - queueing a user callback has to break the wait it is meant to
  // interrupt, and no handle it watches has changed.
  static void NotifyWaitAnyUnconditional() {
    {
      std::lock_guard<std::mutex> lock(wait_any_mutex());
      ++wait_any_generation();
    }
    wait_any_cond().notify_all();
  }
  // The signal path. Callers hold this handle's mutex and have already made the
  // handle signalled, so a scan that misses the notify still sees the state.
  void NotifyWaitAny() {
    if (multi_wait_refs_.load(std::memory_order_seq_cst) == 0) {
      return;
    }
    NotifyWaitAnyUnconditional();
  }

  // Marks handles as multi-waited for as long as a scan is running, so signals
  // to them are worth a notify. Published before the first scan reads any
  // handle state, which is what makes a signal racing the scan safe: either the
  // signaller sees the count and wakes us, or it set the state before we looked.
  class MultiWaitRefs {
   public:
    explicit MultiWaitRefs(const std::vector<PosixConditionBase*>& handles) : handles_(handles) {
      for (auto* handle : handles_) {
        handle->multi_wait_refs_.fetch_add(1, std::memory_order_seq_cst);
      }
    }
    ~MultiWaitRefs() {
      for (auto* handle : handles_) {
        handle->multi_wait_refs_.fetch_sub(1, std::memory_order_seq_cst);
      }
    }
    MultiWaitRefs(const MultiWaitRefs&) = delete;
    MultiWaitRefs& operator=(const MultiWaitRefs&) = delete;

   private:
    const std::vector<PosixConditionBase*>& handles_;
  };

  // `alert_flag`, when given, is the calling thread's pending-user-callback
  // flag. Returning kTimeout as soon as it is set lets an alertable wait pass
  // the whole remaining timeout down instead of slicing it into milliseconds
  // and rescanning between every slice.
  static std::pair<WaitResult, size_t> WaitMultiple(const std::vector<PosixConditionBase*>& handles,
                                                    bool wait_all,
                                                    std::chrono::milliseconds timeout,
                                                    std::atomic<bool>* alert_flag = nullptr) {
    assert_true(!handles.empty());

    if (handles.size() == 1 && alert_flag == nullptr) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    MultiWaitRefs refs(handles);

    // Reused across iterations so a scan does not allocate. Only wait_all needs
    // it; a WaitAny holds one lock at a time.
    std::vector<std::unique_lock<std::mutex>> locks;
    if (wait_all) {
      locks.reserve(handles.size());
    }

    while (true) {
      if (alert_flag != nullptr && alert_flag->load(std::memory_order_acquire)) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      // Read before the scan: a signal that arrives while scanning changes this,
      // and the wait below then returns immediately instead of sleeping through
      // the thing it was waiting for.
      uint64_t generation_before_scan;
      {
        std::lock_guard<std::mutex> lock(wait_any_mutex());
        generation_before_scan = wait_any_generation();
      }

      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;

      if (!wait_all) {
        // One handle at a time. No try-lock, so no contention spin.
        for (size_t i = 0; i < handles.size(); ++i) {
          auto* handle = handles[i];
#if REX_HAS_ROBUST_MUTEX
          auto native_mutex = static_cast<pthread_mutex_t*>(handle->mutex_.native_handle());
          int result = pthread_mutex_lock(native_mutex);
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          } else if (result != 0) {
            continue;
          }
          std::unique_lock<std::mutex> lock(handle->mutex_, std::adopt_lock);
#else
          std::unique_lock<std::mutex> lock(handle->mutex_);
#endif
          if (handle->signaled()) {
            handle->post_execution();
            return std::make_pair(WaitResult::kSuccess, i);
          }
        }
      } else {
        bool all_locked = true;
        locks.clear();
        for (size_t i = 0; i < handles.size(); ++i) {
#if REX_HAS_ROBUST_MUTEX
          auto native_mutex = static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
          int result = pthread_mutex_trylock(native_mutex);
          if (result == 0 || result == EOWNERDEAD) {
            if (result == EOWNERDEAD) {
              pthread_mutex_consistent(native_mutex);
            }
            locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
          } else {
            all_locked = false;
            break;
          }
#else
          locks.emplace_back(handles[i]->mutex_, std::try_to_lock);
          if (!locks.back().owns_lock()) {
            all_locked = false;
            break;
          }
#endif
        }

        if (!all_locked) {
          locks.clear();
          std::this_thread::yield();
          continue;
        }

        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;

        if (condition_met) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
          locks.clear();
          return std::make_pair(WaitResult::kSuccess, first_signaled);
        }

        locks.clear();
      }

      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      // Backstop only. A signal wakes this immediately; the interval bounds how
      // long a wait can hang if some future signal path forgets to notify.
      constexpr auto kRescanBackstop = std::chrono::milliseconds(5);
      auto wait_time = kRescanBackstop;
      if (timeout != std::chrono::milliseconds::max()) {
        wait_time = std::min(
            kRescanBackstop, std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now));
      }
      std::unique_lock<std::mutex> wait_lock(wait_any_mutex());
      if (wait_any_generation() == generation_before_scan) {
        wait_any_cond().wait_for(wait_lock, wait_time);
      }
    }
  }

  virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;
  std::condition_variable cond_;
  std::mutex mutex_;
  // How many multi-handle waits are currently scanning this handle. Read by
  // NotifyWaitAny to decide whether a signal is worth waking them for.
  std::atomic<uint32_t> multi_wait_refs_{0};
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  virtual ~PosixCondition() = default;

  bool Signal() override {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyWaitAny();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = false;
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    cond_.notify_all();
    NotifyWaitAny();
    return true;
  }

 private:
  inline bool signaled() const override { return count_ > 0; }
  inline void post_execution() override {
    count_--;
    cond_.notify_all();
    NotifyWaitAny();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        cond_.notify_all();
        NotifyWaitAny();
      }
      return true;
    }
    return false;
  }

  void* native_handle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  inline bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  inline void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    ++rex_diag_timer_signal;
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyWaitAny();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    ++rex_diag_timer_setonce;
    Cancel();
    ++rex_diag_timer_setonce_armed;

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() {
    ++rex_diag_timer_cancel;
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    ++rex_diag_timer_completion;
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : signaled_(false), exit_code_(0), state_(State::kUninitialized), suspend_count_(0) {
    std::memset(&thread_, 0, sizeof(thread_));
    name_[0] = '\0';
  }

  bool Initialize(Thread::CreationParameters params, ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;

    // The placement map cannot be consulted yet - the thread has no name until
    // it gives itself one - so it starts at the preemptive priority and on
    // whatever core the process defaults to, and moves itself when it is named.
    // Starting anywhere else would let a thread run at a priority that starves
    // its siblings for the moments before it is placed.
    int priority = kPreemptivePriority;
    if (params.initial_priority != 0) {
      priority = MapGuestPriority(params.initial_priority);
    }

    // Page-align: libnx allocates the stack itself when the memory is null, and
    // it requires the size to be a multiple of the page size.
    size_t stack_size = (params.stack_size + 0xFFF) & ~size_t(0xFFF);
    if (stack_size == 0) {
      stack_size = 0x100000;
    }

    Result rc = threadCreate(&thread_, &ThreadEntry, start_data, nullptr, stack_size, priority,
                             -2 /* the process default core */);
    if (R_FAILED(rc)) {
      REXSYS_ERROR("threadCreate failed (rc=0x{:x}) for a {} KB stack at priority {}", rc,
                   stack_size / 1024, priority);
      return false;
    }
    created_ = true;
    if (R_FAILED(threadStart(&thread_))) {
      threadClose(&thread_);
      created_ = false;
      return false;
    }
    return true;
  }

  /// For a thread this library did not create - in practice only the main one,
  /// reached through Thread::GetCurrentThread().
  explicit PosixCondition(::Thread* existing)
      : signaled_(false), exit_code_(0), state_(State::kRunning), suspend_count_(0) {
    std::memset(&thread_, 0, sizeof(thread_));
    if (existing) {
      thread_ = *existing;
    }
    adopted_ = true;
    name_[0] = '\0';
  }

  virtual ~PosixCondition() {
    // Deliberately does not join or cancel. Horizon cannot cancel a thread at
    // all, and joining from the destructor can self-join depending on shutdown
    // ordering. Threads are stopped explicitly, and the process exits with
    // _Exit, which is what actually reclaims them.
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    std::lock_guard<std::mutex> lock(name_mutex_);
    return std::string(name_);
  }

  void set_name(const std::string& name) {
    WaitStarted();
    {
      std::lock_guard<std::mutex> lock(name_mutex_);
      rex::string::util_copy_truncating(name_, name, rex::countof(name_));
    }
    // Horizon has no kernel-visible thread name, but it can certainly place
    // another thread: svcSetThreadCoreMask and svcSetThreadPriority take a
    // handle and accept any thread of this process. Naming is the only moment a
    // thread's role is known, whoever does the naming, so place it here either
    // way. Most threads are named by their parent - the audio pump, the
    // pipeline compilers, every guest thread the title leaves unnamed - and
    // skipping those left them all at priority 59 on the process default core.
    ApplyPlacementForThread(native_thread_handle(), name);
  }

  uint32_t system_id() const {
    u64 thread_id = 0;
    if (R_FAILED(svcGetThreadId(&thread_id, native_thread_handle()))) {
      return 0;
    }
    return static_cast<uint32_t>(thread_id);
  }

  uint64_t affinity_mask() {
    WaitStarted();
    s32 ideal_core = 0;
    u64 mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&ideal_core, &mask, native_thread_handle()))) {
      return 0;
    }
    return mask;
  }

  void set_affinity_mask(uint64_t mask) {
    WaitStarted();
    // Core 3 belongs to the system. A guest mask that names it would silently
    // pin work onto a core this process can never run on.
    const u32 usable = static_cast<u32>(mask) & kAllCoresMask;
    if (!usable) {
      return;
    }
    // An ideal core of -1 means "no preference beyond the mask", which is the
    // right reading of a multi-core mask.
    s32 ideal = -1;
    if ((usable & (usable - 1)) == 0) {
      ideal = __builtin_ctz(usable);
    }
    svcSetThreadCoreMask(native_thread_handle(), ideal, usable);
  }

  int priority() {
    WaitStarted();
    s32 value = 0;
    if (R_FAILED(svcGetThreadPriority(&value, native_thread_handle()))) {
      return -1;
    }
    return UnmapGuestPriority(static_cast<int>(value));
  }

  void set_priority(int new_priority) {
    WaitStarted();
    // A thread the map names keeps what the map gave it. Two callers raise a
    // priority just after creation - the XMA decoder and the audio worker both
    // ask for "above normal" from the parent thread - and that arrives after
    // the placement, silently undoing it. Which of the two lands last was a
    // race, so the thread's actual priority varied from run to run.
    {
      std::string current;
      {
        std::lock_guard<std::mutex> lock(name_mutex_);
        current = name_;
      }
      const Placement p = LookUpPlacement(current);
      if (p.matched && p.priority >= 0) {
        return;
      }
    }
    svcSetThreadPriority(native_thread_handle(), MapGuestPriority(new_priority));
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true, std::memory_order_release);
    }

    if (IsCurrentThread()) {
      if (alertable_state_) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

    // On POSIX a signal nudges the target thread awake. There are no signals
    // here, and none are needed: alertable waits poll the flag set above on a
    // one-millisecond slice, and this wakes any multi-handle scan that is
    // sleeping on a set of handles none of which has changed.
    PosixConditionBase::NotifyWaitAnyUnconditional();
  }

  std::atomic<bool>* user_callback_flag() { return &has_pending_user_callbacks_; }

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      if (user_callbacks_.empty()) {
        has_pending_user_callbacks_.store(false, std::memory_order_release);
        return false;
      }
      callback = std::move(user_callbacks_.front());
      user_callbacks_.pop_front();
      has_pending_user_callbacks_.store(!user_callbacks_.empty(), std::memory_order_release);
    }
    if (callback) {
      callback();
    }
    return true;
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool wake_self = false;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (suspend_count_ == 0) {
        return false;
      }
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      --suspend_count_;
      if (suspend_count_ == 0 && state_ == State::kSuspended) {
        state_ = State::kRunning;
        wake_self = true;
      }
      state_signal_.notify_all();
    }
    if (wake_self && created_) {
      threadResume(&thread_);
    }
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    const bool is_current_thread = IsCurrentThread();
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    if (is_current_thread) {
      // Suspending yourself waits on the condition variable rather than the
      // kernel: a thread cannot ask the kernel to stop itself and then be the
      // one to notice it was resumed.
      WaitSuspended();
      return true;
    }
    if (!created_) {
      return false;
    }
    return R_SUCCEEDED(threadPause(&thread_));
  }

  void Terminate(int exit_code) {
    const bool is_current_thread = IsCurrentThread();
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          assert_always();
          for (;;) {
          }
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      exit_code_ = exit_code;
      signaled_ = true;
      cond_.notify_all();
      NotifyWaitAny();
    }

    if (is_current_thread) {
      threadExit();
    }
    // Horizon has nothing like pthread_cancel: a thread can only be stopped by
    // returning from its own entry point. The best available is to pause it and
    // treat it as gone. It keeps its stack until the process exits, which is
    // acceptable because this is only reached during shutdown.
    if (created_) {
      threadPause(&thread_);
    }
  }

  void WaitStarted() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  void WaitSuspended() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return suspend_count_ == 0; });
  }

  void* native_handle() const override {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(native_thread_handle()));
  }

  // The guest's five priority levels (1, 8, 16, 24, 32 - see rex/thread.h) run
  // the opposite way to Horizon's, where a smaller number is more urgent. They
  // are also clamped at the bottom: nothing may sit below the preemptive level,
  // because a thread there would run until it blocked and starve every sibling
  // on its core.
  static int MapGuestPriority(int guest_priority) {
    if (guest_priority >= 32) {
      return 0x2E;  // above the main thread, for the few genuinely urgent ones
    }
    if (guest_priority >= 24) {
      return 0x34;
    }
    return kPreemptivePriority;
  }

  static int UnmapGuestPriority(int horizon_priority) {
    if (horizon_priority <= 0x2E) {
      return 32;
    }
    if (horizon_priority <= 0x34) {
      return 24;
    }
    return 16;
  }

 private:
  friend class PosixThread;
  static void ThreadEntry(void* parameter);

  Handle native_thread_handle() const {
    if (adopted_ || !created_) {
      return CUR_THREAD_HANDLE;
    }
    return thread_.handle;
  }

  bool IsCurrentThread() const {
    if (adopted_ || !created_) {
      return threadGetSelf() == nullptr || threadGetCurHandle() == thread_.handle;
    }
    return threadGetCurHandle() == thread_.handle;
  }

  inline bool signaled() const override { return signaled_; }
  inline void post_execution() override {
    if (created_) {
      threadWaitForExit(&thread_);
      threadClose(&thread_);
      created_ = false;
    }
  }

  ::Thread thread_;
  bool created_ = false;
  bool adopted_ = false;
  bool signaled_;
  int exit_code_;
  State state_;             // Protected by state_mutex_
  uint32_t suspend_count_;  // Protected by state_mutex_
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::mutex name_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<std::function<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
  char name_[32];
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
}

std::atomic<bool>* CurrentThreadUserCallbackFlag() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ ? current_thread_condition_->user_callback_flag() : nullptr;
}

namespace {

constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      alertable_state_ = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      alertable_state_ = false;
    }
  }

 private:
  bool alertable_;
};

std::chrono::steady_clock::time_point ComputeAlertableDeadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + timeout;
}

bool HasAlertableTimeoutElapsed(std::chrono::steady_clock::time_point deadline) {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

std::chrono::milliseconds ComputeRemainingWaitTimeout(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return std::chrono::milliseconds::max();
  }
  auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  return remaining <= std::chrono::milliseconds::zero() ? std::chrono::milliseconds::zero()
                                                        : remaining;
}

std::chrono::milliseconds ComputeAlertableWaitTimeout(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return kAlertablePollSlice;
  }
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return std::min(remaining, kAlertablePollSlice);
}

}  // namespace

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(::Thread* thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  void* native_handle() const override { return handle_.native_handle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner) : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset) : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset, bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(::Thread* thread) : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (!is_alertable) {
    return posix_wait_handle->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);

  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    auto result = posix_wait_handle->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->condition().Signal()) {
    return WaitResult::kFailed;
  }

  if (!is_alertable) {
    return posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    result = posix_wait_handle_to_wait_on->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(conditions, wait_all, timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  auto deadline = ComputeAlertableDeadline(timeout);
  // With the flag in hand the wait returns as soon as a callback is queued, so
  // it can be given the whole remaining timeout. Without it - a host thread with
  // no condition of its own - fall back to slicing, which is correct but wakes
  // a thousand times a second.
  std::atomic<bool>* alert_flag = CurrentThreadUserCallbackFlag();
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
    auto slice = alert_flag != nullptr ? ComputeRemainingWaitTimeout(deadline)
                                       : ComputeAlertableWaitTimeout(deadline);
    auto result = PosixConditionBase::WaitMultiple(conditions, wait_all, slice, alert_flag);
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
  }
}

class PosixEvent : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count), out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner) : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer : public PosixConditionHandle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}


class PosixThread : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(::Thread* thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params, std::function<void()> start_routine) {
    auto start_data = new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override { handle_.set_affinity_mask(mask); }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override { handle_.set_priority(new_priority); }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
};

thread_local PosixThread* current_thread_ = nullptr;

void PosixCondition<Thread>::ThreadEntry(void* parameter) {
  set_current_thread_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  current_thread_condition_ = &thread->handle_;
  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = create_suspended ? State::kSuspended : State::kRunning;
    // The suspend count MUST be published in the same critical section that
    // announces the thread has started. Setting it in a second lock scope left
    // a window in which Resume() saw a count of zero and did nothing, after
    // which the thread set the count to 1 and waited for a resume that had
    // already come and gone - a thread that never ran an instruction, and a
    // boot that sits on a black screen.
    if (create_suspended) {
      thread->handle_.suspend_count_ = 1;
    }
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_signal_.wait(lock,
                                       [thread] { return thread->handle_.suspend_count_ == 0; });
  }

  start_routine();

  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.cond_.notify_all();
  }

  current_thread_ = nullptr;
  current_thread_condition_ = nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Only reached for a thread this library did not create, which in practice
  // means the main one.
  current_thread_ = new PosixThread(threadGetSelf());
  current_thread_condition_ = &current_thread_->condition();
  // Deliberately not deleted: it is thread-local and freeing it from a
  // destructor at exit trips an assert. The leak is bounded by the thread count.
  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // The main thread. threadExit here would end the process without letting
    // the applet loop release its exit lock, so leave it to the caller.
    (void)exit_code;
  }
  assert_always();
}

void set_current_thread_name(const std::string_view name) {
  // Horizon threads have no kernel-visible name to set. The name is recorded on
  // the thread object, and it is what drives placement.
  if (current_thread_) {
    current_thread_->condition().set_name(std::string(name));
  } else {
    ApplyPlacementForCurrentThreadName(name);
  }
}

}  // namespace rex::thread
