/**
 * @file        rex/execinfo_android.h
 * @brief       <execinfo.h> stand-in for Android
 *
 * bionic ships no <execinfo.h>, and neither does the newlib devkitA64 uses for
 * Horizon. These provide the same three entry points on top of the unwinder, so
 * crash reporting and heap tracing keep working unmodified on both. The header
 * keeps its Android name because that is what the call sites already include.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID || REX_PLATFORM_SWITCH

// Deliberately in the global namespace and with these exact signatures so that
// existing ::backtrace() call sites need no changes.
int backtrace(void** buffer, int size);
char** backtrace_symbols(void* const* buffer, int size);
void backtrace_symbols_fd(void* const* buffer, int size, int fd);

#endif  // REX_PLATFORM_ANDROID || REX_PLATFORM_SWITCH
