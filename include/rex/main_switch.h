/**
 * @file        rex/main_switch.h
 * @brief       Horizon application bootstrap.
 *
 * The Android equivalent exists because the JVM owns the process and hands the
 * native side a context. Here nothing owns the process but us, so this is
 * where the services the runtime assumes are brought up - the SD card, the
 * pads, the network for nxlink - and where the one precondition this port
 * cannot work without is checked.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <cstdint>

namespace rex {

// Brings up libnx services, installs the guest memory reservation and the
// thread bookkeeping, and redirects stdout to nxlink when a host is listening.
// Must run before any other rex subsystem. Returns false only for a condition
// the process cannot continue past; it reports the reason itself.
bool InitializeSwitchApp();
void ShutdownSwitchApp();

// True when the process was launched with an application's memory pool and
// syscall set - in practice, through hbmenu's title override rather than from
// the album. Everything about this port's memory layout depends on it.
bool IsSwitchApplicationMode();

// Total and in-use memory of this process's pool, in bytes, straight from the
// kernel. Used for the periodic memory line and to pick cache budgets.
uint64_t SwitchTotalMemory();
uint64_t SwitchUsedMemory();

// True when stdout is going to a listening nxlink host rather than a file.
bool SwitchHasNxlinkStdio();

// Commits everything written to the log so far to the SD card. Safe to call
// from an exception handler; without it a crash truncates the log at whatever
// the filesystem last committed.
void SwitchFlushLog();

}  // namespace rex

#endif  // REX_PLATFORM_SWITCH
