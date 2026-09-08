/**
 * @file        rex/core/main_switch.cpp
 * @brief       Horizon application bootstrap.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <rex/main_switch.h>

#include <switch.h>

#include <cstdio>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <filesystem>
#include <cstdlib>

#include <rex/memory/utils.h>
#include <rex/thread.h>

extern "C" {

// Let libnx work out how it was launched. NXVK's sample forces
// AppletType_Application, which is right for the memory pool and wrong for
// everything else: forced from the album it breaks appletInitialize outright.
// The mode is checked below instead, where a bad one can be explained.
//
// __nx_heap_size 0 asks libnx for the whole pool rather than a fixed slice.
// The guest reservation, the caches and every thread stack come out of it.
u32 __nx_applet_type = AppletType_Default;
size_t __nx_heap_size = 0;

// The exception handler runs on its own stack, because the faulting thread's
// is often the thing that is broken. 16 KB is enough for the register dump and
// the backtrace walk in exception_handler_switch.cpp, both of which are
// written not to allocate.
// Set once stderr points at the SD card; -1 while it still points at nxlink or
// nowhere. Read by the sync thread and by SwitchFlushLog.
std::atomic<int> stderr_fd_{-1};

alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

}  // extern "C"

namespace rex {

namespace {

bool socket_ready_ = false;
bool nxlink_stdio_ = false;
bool application_mode_ = false;
int nxlink_socket_ = -1;

uint64_t QueryInfo(InfoType type) {
  u64 value = 0;
  if (R_FAILED(svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0))) {
    return 0;
  }
  return value;
}

}  // namespace

bool InitializeSwitchApp() {
  const AppletType applet_type = appletGetAppletType();
  application_mode_ =
      applet_type == AppletType_Application || applet_type == AppletType_SystemApplication;

  // Networking first, so that everything after this point - including the
  // failure below - is visible on the development machine.
  if (R_SUCCEEDED(socketInitializeDefault())) {
    socket_ready_ = true;
    nxlink_socket_ = nxlinkStdio();
    nxlink_stdio_ = nxlink_socket_ >= 0;
  }

  // Point stderr at a file before anything is written to it. This used to
  // happen in main() after this function returned, so everything below - the
  // applet-mode refusal and the image base among it - went to a stderr nobody
  // was reading and never reached the log.
  //
  // Unbuffered, and separately synced: line buffering is not enough here. A
  // flush only hands the bytes to the SD filesystem, which does not commit the
  // file's new size until the handle is synced or closed. A process that dies
  // without closing loses everything written since the last commit, so the log
  // stops at a filesystem boundary rather than at the fault - which is exactly
  // as misleading as it sounds when the log is all you have.
  if (!nxlink_stdio_) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/switch/skate3", ec);
    if (std::freopen("sdmc:/switch/skate3/stderr.log", "w", stderr)) {
      setvbuf(stderr, nullptr, _IOLBF, 0);
      stderr_fd_ = fileno(stderr);
    }
  }

  if (!application_mode_) {
    // Not a warning: in applet mode this process gets a few hundred megabytes
    // and none of the process-memory syscalls the guest address space is built
    // from, so there is no point continuing to a failure further in that would
    // be harder to read.
    std::fprintf(stderr,
                 "[boot] Skate 3 needs to run as an application, not an applet.\n"
                 "[boot] Launch it from hbmenu opened over a game: hold R while starting any\n"
                 "[boot] installed title, then pick Skate 3. Starting hbmenu from the album\n"
                 "[boot] gives this process too little memory to load the game.\n");
    std::fflush(stderr);
    return false;
  }

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  hidInitializeTouchScreen();

  // Home and the power menu must not be able to kill the process while the
  // guest is mid-frame with the GPU holding buffers; the applet loop releases
  // this when it is ready to exit.
  appletLockExit();

  // Where the image actually landed. hbloader does not register the NRO as a
  // module, so a system crash report gives raw addresses with nothing to
  // subtract, and this is what turns them back into something addr2line can
  // resolve against skate3.debug.elf.
  //
  // The address of a real function rather than the linker's __start__: that
  // symbol resolved to zero here, which for a position-independent image is a
  // plausible thing for it to mean and useless for this purpose. A function's
  // address is unambiguous - subtract its address in the ELF, given by nm, and
  // the difference is the relocation applied to everything.
  std::fprintf(stderr, "[boot] &InitializeSwitchApp = %p (see scripts/symbolize_crash.sh)\n",
               (void*)&InitializeSwitchApp);

  // Walk this process's own mapping across the image and report what the
  // kernel actually gave us.
  //
  // Every crash so far is a call into a function sitting in the last few
  // kilobytes of a 76 MB .text, landing instead at the image base. The same
  // calls work perfectly in a small test binary, which points at the loading of
  // something this large rather than at the code. hbloader's own source carries
  // a "todo: Detect whether NRO fits into heap or not", so it is worth asking
  // the kernel directly whether the whole image is mapped and executable.
  {
    const u64 anchor = (u64)&InitializeSwitchApp;
    std::fprintf(stderr, "[map] walking the image from the anchor\n");
    u64 addr = anchor & ~0xFFFFFull;  // back off to a round address below it
    // Far enough to cross a 76 MB text section and its neighbours.
    const u64 limit = addr + 0x8000000ull;
    int regions = 0;
    while (addr < limit && regions < 24) {
      MemoryInfo info = {};
      u32 pageinfo = 0;
      if (R_FAILED(svcQueryMemory(&info, &pageinfo, addr))) {
        std::fprintf(stderr, "[map] query failed at %#llx\n", (unsigned long long)addr);
        break;
      }
      if (info.type == MemType_Unmapped && info.size == 0) {
        break;
      }
      const char* kind = info.perm == Perm_Rx   ? "r-x"
                         : info.perm == Perm_R  ? "r--"
                         : info.perm == Perm_Rw ? "rw-"
                         : info.perm == Perm_None ? "---" : "?";
      std::fprintf(stderr, "[map] %#012llx +%#010llx %s type=%u%s\n",
                   (unsigned long long)info.addr, (unsigned long long)info.size, kind,
                   (unsigned)info.type,
                   (anchor >= info.addr && anchor < info.addr + info.size) ? "  <- code is here"
                                                                          : "");
      ++regions;
      const u64 next = info.addr + info.size;
      if (next <= addr) break;
      addr = next;
    }
  }

  std::fprintf(stderr, "[boot] Horizon %u.%u.%u, %s, pool %llu MiB (%llu MiB used)\n",
               (unsigned)HOSVER_MAJOR(hosversionGet()), (unsigned)HOSVER_MINOR(hosversionGet()),
               (unsigned)HOSVER_MICRO(hosversionGet()),
               appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
               (unsigned long long)(SwitchTotalMemory() >> 20),
               (unsigned long long)(SwitchUsedMemory() >> 20));

  rex::memory::SwitchInitialize();
  rex::thread::SwitchInitialize();
  return true;
}

void ShutdownSwitchApp() {
  rex::thread::SwitchShutdown();
  rex::memory::SwitchShutdown();

  appletUnlockExit();

  if (socket_ready_) {
    if (nxlink_socket_ >= 0) {
      close(nxlink_socket_);
      nxlink_socket_ = -1;
    }
    socketExit();
    socket_ready_ = false;
    nxlink_stdio_ = false;
  }
}

bool IsSwitchApplicationMode() { return application_mode_; }

uint64_t SwitchTotalMemory() { return QueryInfo(InfoType_TotalMemorySize); }

uint64_t SwitchUsedMemory() { return QueryInfo(InfoType_UsedMemorySize); }

bool SwitchHasNxlinkStdio() { return nxlink_stdio_; }

}  // namespace rex

#endif  // REX_PLATFORM_SWITCH

namespace rex {

void SwitchFlushLog() {
  const int fd = stderr_fd_.load();
  if (fd < 0) {
    std::fflush(stderr);
    std::fflush(stdout);
    return;
  }
  // fsync bypasses stdio and talks to the filesystem directly, so it must not
  // run while another thread is part-way through a write to the same handle.
  flockfile(stderr);
  std::fflush(stderr);
  // Commits the file's size, not just its bytes. Without this a crash leaves
  // the log looking as though execution stopped wherever the filesystem last
  // committed, which is not where the fault is.
  fsync(fd);
  funlockfile(stderr);
  std::fflush(stdout);
}

}  // namespace rex
