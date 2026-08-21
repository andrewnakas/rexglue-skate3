/**
 * @file        rex/core/fiber_aarch64.cpp
 * @brief       AArch64 backend for rex::thread::Fiber (Android and iOS)
 *
 * Bionic declares <ucontext.h> but implements none of getcontext/setcontext/
 * makecontext/swapcontext, and the iOS SDK does not provide them at all, so
 * fiber_posix.cpp cannot be used on either. This backend keeps the same
 * semantics on top of the AArch64 primitives in fiber_aarch64.S.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID || REX_PLATFORM_IOS

#include <rex/thread/fiber.h>

#include <cassert>

extern "C" {
void rex_fiber_switch(void** from_sp, void* to_sp);
void* rex_fiber_make_context(void* stack_top, void (*entry)(void));
}

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  // sp_ is filled in by the first SwitchTo away from this fiber; until then
  // there is nothing to resume, because we are already running on it.
  f->sp_ = nullptr;
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  // Stacks grow down, so the context starts at the top of the buffer.
  f->sp_ = rex_fiber_make_context(f->stack_.data() + f->stack_.size(), &Fiber::Trampoline);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before the switch landed here, which
  // is why entry_/arg_ need no pointer splitting through the asm.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  // entry_ is not expected to return; there is no fiber to fall back to.
  assert(false && "fiber entry point returned");
  for (;;) {
  }
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  if (from == target) {
    return;
  }
  tls_current_ = target;
  rex_fiber_switch(&from->sp_, target->sp_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_ANDROID || REX_PLATFORM_IOS
