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
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

}  // extern "C"

// Placed by libnx's linker script at the start of the loaded image.
extern "C" char __start__;

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
  // was reading and never reached the log. Line-buffered, so a crash loses at
  // most the line in progress.
  if (!nxlink_stdio_) {
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/switch/skate3", ec);
    if (std::freopen("sdmc:/switch/skate3/stderr.log", "w", stderr)) {
      setvbuf(stderr, nullptr, _IOLBF, 0);
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

  // The address the image was loaded at. hbloader does not register the NRO as
  // a module, so a system crash report gives raw addresses with nothing to
  // subtract - and this is the number that turns them back into offsets that
  // addr2line can resolve against skate3.debug.elf. Printed first, because a
  // crash before anything else still needs it.
  std::fprintf(stderr, "[boot] image base: %p (subtract this from a crash PC)\n",
               (void*)&__start__);

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
