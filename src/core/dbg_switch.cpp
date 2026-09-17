/**
 * Debugger interaction on Horizon.
 *
 * The POSIX file reads /proc/self/status and raises SIGTRAP. Neither exists
 * here: there is no procfs, and Horizon has no signals at all. The kernel
 * answers the first question directly, and svcBreak is the trap.
 */

#include <cstdio>
#include <cstring>

#include <switch.h>

#include <rex/dbg.h>
#include <rex/string/buffer.h>

namespace rex::debug {

bool IsDebuggerAttached() {
  u8 debugged = 0;
  // InfoType_DebuggerAttached reports whether a debugger is presently attached
  // to this process. Anything other than success means no.
  if (R_FAILED(svcGetInfo(reinterpret_cast<u64*>(&debugged), InfoType_DebuggerAttached,
                          CUR_PROCESS_HANDLE, 0))) {
    return false;
  }
  return debugged != 0;
}

void Break() {
  // With a debugger attached this stops in it. Without one it terminates the
  // process, which is the same thing the POSIX path does once SIGTRAP reaches
  // the default handler.
  svcBreak(BreakReason_User, 0, 0);
}

namespace detail {
void DebugPrint(const char* s) {
  // Two destinations on purpose. stderr reaches nxlink and the log file, which
  // is what anyone reads; the kernel's debug string reaches an attached
  // debugger, which is the only thing still listening once the process is too
  // broken to flush its own streams.
  std::fputs(s, stderr);
  std::fputc('\n', stderr);
  svcOutputDebugString(s, std::strlen(s));
}
}  // namespace detail

}  // namespace rex::debug
