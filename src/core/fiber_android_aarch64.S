/**
 * @file        rex/core/fiber_android_aarch64.S
 * @brief       AArch64 cooperative context switch for rex::thread::Fiber
 *
 * Bionic implements neither makecontext() nor swapcontext(), so the POSIX
 * fiber backend cannot be used on Android. This provides the two primitives
 * fiber_android.cpp builds on.
 *
 * Saves exactly the AAPCS64 callee-saved state: x19-x28, x29 (FP), x30 (LR)
 * and the low 64 bits of v8-v15. Caller-saved registers are, by definition,
 * already dead across the call. 160-byte frame, 16-byte aligned.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#if defined(__aarch64__)

#define FRAME_SIZE 160

    .text

/**
 * void rex_fiber_switch(void** from_sp, void* to_sp);
 *   x0 = where to store the outgoing stack pointer
 *   x1 = stack pointer to resume
 *
 * Returns to whatever pushed to_sp - either a previous rex_fiber_switch, or
 * rex_fiber_make_context for a fiber being entered the first time.
 */
    .global rex_fiber_switch
    .hidden rex_fiber_switch
    .type rex_fiber_switch, %function
    .balign 16
rex_fiber_switch:
    .cfi_startproc
    sub     sp,  sp,  #FRAME_SIZE
    .cfi_def_cfa_offset FRAME_SIZE
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    .cfi_rel_offset x29, 80
    .cfi_rel_offset x30, 88
    stp     d8,  d9,  [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]

    mov     x2,  sp
    str     x2,  [x0]           // *from_sp = sp
    mov     sp,  x1             // switch stacks

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    ldp     d8,  d9,  [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]
    add     sp,  sp,  #FRAME_SIZE
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
    .size rex_fiber_switch, .-rex_fiber_switch

/**
 * void* rex_fiber_make_context(void* stack_top, void (*entry)(void));
 *   x0 = highest address of the fiber's stack (will be 16-byte aligned down)
 *   x1 = entry point, called with a null frame pointer and never returned to
 *
 * Returns the stack pointer to hand to rex_fiber_switch's to_sp. The first
 * switch into it pops this frame and 'ret's straight into entry.
 */
    .global rex_fiber_make_context
    .hidden rex_fiber_make_context
    .type rex_fiber_make_context, %function
    .balign 16
rex_fiber_make_context:
    .cfi_startproc
    and     x0,  x0,  #~15      // AAPCS64 requires 16-byte SP alignment
    sub     x0,  x0,  #FRAME_SIZE
    stp     xzr, xzr, [x0, #0]
    stp     xzr, xzr, [x0, #16]
    stp     xzr, xzr, [x0, #32]
    stp     xzr, xzr, [x0, #48]
    stp     xzr, xzr, [x0, #64]
    // x29 = 0 terminates any stack walk at the fiber entry; x30 = entry, so
    // the 'ret' at the end of rex_fiber_switch lands on it.
    stp     xzr, x1,  [x0, #80]
    stp     d8,  d9,  [x0, #96]     // contents irrelevant, entry clobbers them
    stp     d10, d11, [x0, #112]
    stp     d12, d13, [x0, #128]
    stp     d14, d15, [x0, #144]
    ret
    .cfi_endproc
    .size rex_fiber_make_context, .-rex_fiber_make_context

// Never need an executable stack for this object.
.section .note.GNU-stack,"",%progbits

#endif  // __aarch64__
