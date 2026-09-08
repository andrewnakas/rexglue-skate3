/**
 * @file        core/exception_handler_switch.cpp
 * @brief       Fault reporting on Horizon.
 *
 * On every other platform this file is load-bearing: a fault arrives as a
 * signal, a handler inspects it, and for a guest write into a watched page it
 * fixes things up and lets the thread carry on. That is how the emulated
 * renderer learns the CPU touched a texture.
 *
 * None of that is possible here. Horizon delivers a fault by running
 * __libnx_exception_handler on a dedicated stack, and there is no supported way
 * to resume the thread that faulted - returning from the handler ends the
 * process. So a fault is always fatal, and the only useful thing to do with it
 * is describe it well enough to fix offline.
 *
 * Installed handlers are still called, in order, because several of them do
 * useful reporting (the guest backchain walk, the crash report). Their return
 * value, which elsewhere means "handled, resume", is recorded and ignored.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/exception_handler.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>
#include <rex/main_switch.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/math.h>
#include <rex/platform.h>

namespace rex::arch {

namespace {

// Matches the POSIX file: enough for the handful of subsystems that register,
// scanned linearly on a path that runs once per process lifetime.
constexpr size_t kMaxHandlerCount = 8;
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

// Exception class values from ESR_EL1 bits 31:26.
constexpr uint32_t kEcDataAbortLowerEl = 0b100100;
constexpr uint32_t kEcDataAbortSameEl = 0b100101;
constexpr uint32_t kEcInstructionAbortLowerEl = 0b100000;
constexpr uint32_t kEcInstructionAbortSameEl = 0b100001;

const char* DescribeExceptionClass(uint32_t ec) {
  switch (ec) {
    case kEcDataAbortLowerEl:
    case kEcDataAbortSameEl:
      return "data abort";
    case kEcInstructionAbortLowerEl:
    case kEcInstructionAbortSameEl:
      return "instruction abort";
    case 0b000000:
      return "unknown reason";
    case 0b000111:
      return "SIMD/FP access trapped";
    case 0b001110:
      return "illegal execution state";
    case 0b010001:
    case 0b010101:
      return "SVC";
    case 0b011000:
      return "MSR/MRS trapped";
    case 0b100010:
      return "PC alignment fault";
    case 0b100110:
      return "SP alignment fault";
    case 0b101000:
    case 0b101100:
      return "floating-point exception";
    case 0b110000:
    case 0b110001:
      return "breakpoint";
    case 0b111100:
      return "BRK instruction";
    default:
      return "unclassified";
  }
}

}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < rex::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      return;
    }
  }
}

}  // namespace rex::arch

// Visibility, explicitly. libnx declares this weak and the kernel calls
// whatever the link resolved it to; the SDK compiles with -fvisibility=hidden,
// which made this local and left libnx's default in place - so every fault
// terminated the process silently instead of reporting itself.
extern "C" __attribute__((visibility("default"))) void __libnx_exception_handler(
    ThreadExceptionDump* ctx) {
  using namespace rex::arch;

  // Re-entering means a handler faulted. There is no recovering from that and
  // no report worth waiting for, so end it immediately.
  static volatile bool in_handler = false;
  if (in_handler) {
    svcBreak(BreakReason_Panic, 0, 0);
    while (true) {
    }
  }
  in_handler = true;

  HostThreadContext thread_context{};
  for (size_t i = 0; i < 29; ++i) {
    thread_context.x[i] = ctx->cpu_gprs[i].x;
  }
  thread_context.x[29] = ctx->fp.x;
  thread_context.x[30] = ctx->lr.x;
  thread_context.sp = ctx->sp.x;
  thread_context.pc = ctx->pc.x;
  thread_context.pstate = ctx->pstate;
  // Left at zero: the kernel's exception dump carries the NEON registers but
  // not the floating-point status and control words. They appear in a full
  // ThreadContext, which is what a debugger reads, not in what a faulting
  // thread is handed.
  thread_context.fpsr = 0;
  thread_context.fpcr = 0;
  for (size_t i = 0; i < 32; ++i) {
    std::memcpy(&thread_context.v[i], &ctx->fpu_gprs[i], sizeof(thread_context.v[i]));
  }

  const uint32_t esr = ctx->esr;
  const uint32_t ec = (esr >> 26) & 0b111111;
  const uint64_t fault_address = ctx->far.x;
  const bool is_abort = ec == kEcDataAbortLowerEl || ec == kEcDataAbortSameEl ||
                        ec == kEcInstructionAbortLowerEl || ec == kEcInstructionAbortSameEl;

  // stderr rather than the logger: this can be reached with the logging lock
  // held by the thread that just died, and a deadlock here loses the report.
  std::fprintf(stderr,
               "\n[fault] %s (error_desc 0x%x, ESR 0x%08x, EC 0x%02x) at pc 0x%016llx, "
               "faulting address 0x%016llx\n",
               DescribeExceptionClass(ec), (unsigned)ctx->error_desc, esr, ec,
               (unsigned long long)thread_context.pc, (unsigned long long)fault_address);
  for (size_t i = 0; i < 31; i += 4) {
    std::fprintf(stderr, "[fault] x%-2zu %016llx  x%-2zu %016llx  x%-2zu %016llx  x%-2zu %016llx\n",
                 i, (unsigned long long)thread_context.x[i], i + 1,
                 (unsigned long long)(i + 1 < 31 ? thread_context.x[i + 1] : 0), i + 2,
                 (unsigned long long)(i + 2 < 31 ? thread_context.x[i + 2] : 0), i + 3,
                 (unsigned long long)(i + 3 < 31 ? thread_context.x[i + 3] : 0));
  }
  std::fprintf(stderr, "[fault] sp %016llx  pstate %08x\n", (unsigned long long)thread_context.sp,
               (unsigned)thread_context.pstate);

  // Horizon puts no guard page below a thread stack, so running off the end of
  // one does not fault where it happens - it quietly overwrites what is mapped
  // below and the thread dies later at a nonsense address. Reporting how much
  // room was left turns that into something readable: a few hundred bytes of
  // headroom here means the stack was the cause, whatever the fault says.
  {
    MemoryInfo si = {};
    u32 spi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&si, &spi, thread_context.sp)) && si.size) {
      const u64 sp = thread_context.sp;
      if (sp >= si.addr && sp < si.addr + si.size) {
        const u64 remaining = sp - si.addr;
        std::fprintf(stderr,
                     "[fault] stack %#llx..%#llx (%llu KB), sp is %llu KB above the bottom%s\n",
                     (unsigned long long)si.addr, (unsigned long long)(si.addr + si.size),
                     (unsigned long long)(si.size >> 10), (unsigned long long)(remaining >> 10),
                     remaining < 0x4000 ? "  <-- almost certainly a STACK OVERFLOW" : "");
      } else {
        std::fprintf(stderr, "[fault] sp %#llx is outside any mapped region\n",
                     (unsigned long long)sp);
      }
    }
  }
  std::fflush(stderr);
  rex::SwitchFlushLog();

  Exception ex;
  if (is_abort) {
    // For a data abort, ESR bit 6 says which direction the access was.
    auto operation = Exception::AccessViolationOperation::kUnknown;
    if (ec == kEcDataAbortLowerEl || ec == kEcDataAbortSameEl) {
      operation = (esr & (UINT64_C(1) << 6)) ? Exception::AccessViolationOperation::kWrite
                                             : Exception::AccessViolationOperation::kRead;
    }
    ex.InitializeAccessViolation(&thread_context, fault_address, operation);
  } else {
    ex.InitializeIllegalInstruction(&thread_context);
  }

  // The installed handlers report; none of them can rescue this thread. Their
  // "handled" answer is logged because a handler claiming it could have
  // continued is a useful hint about what the fault was.
  for (size_t i = 0; i < rex::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      std::fprintf(stderr,
                   "[fault] handler %zu would have resumed here; Horizon cannot, so this is "
                   "fatal.\n",
                   i);
      std::fflush(stderr);
      rex::SwitchFlushLog();
    }
  }

  rex::FlushLogging();
  std::fflush(nullptr);
  rex::SwitchFlushLog();

  // BreakReason_Panic stops in an attached debugger and otherwise ends the
  // process with a report the system records.
  svcBreak(BreakReason_Panic, 0, 0);
  while (true) {
  }
}

#endif  // REX_PLATFORM_SWITCH
