/**
 * @file        rex/atomic_ref.h
 * @brief       std::atomic_ref with a fallback for toolchains that lack it
 *
 * libc++ only gained std::atomic_ref in LLVM 19; the Android NDK r27 toolchain
 * ships Clang 18, where it is missing entirely (and -fexperimental-library does
 * not help). This provides the subset rex uses on top of the __atomic builtins,
 * which Clang supports on any suitably aligned lvalue.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the root for full license text.
 */

#pragma once

#include <atomic>
#include <version>

namespace rex {

#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L

template <typename T>
using AtomicRef = std::atomic_ref<T>;

#else

template <typename T>
class AtomicRef {
 public:
  static_assert(std::is_trivially_copyable_v<T>, "AtomicRef requires a trivially copyable type");

  explicit AtomicRef(T& obj) noexcept : ptr_(&obj) {}
  AtomicRef(const AtomicRef&) noexcept = default;

  T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    T out;
    __atomic_load(ptr_, &out, ToBuiltin(order));
    return out;
  }

  void store(T desired, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    __atomic_store(ptr_, &desired, ToBuiltin(order));
  }

  bool compare_exchange_weak(T& expected, T desired, std::memory_order success,
                             std::memory_order failure) const noexcept {
    return __atomic_compare_exchange(ptr_, &expected, &desired, /*weak=*/true,
                                     ToBuiltin(success), ToBuiltin(failure));
  }

  bool compare_exchange_strong(T& expected, T desired, std::memory_order success,
                               std::memory_order failure) const noexcept {
    return __atomic_compare_exchange(ptr_, &expected, &desired, /*weak=*/false,
                                     ToBuiltin(success), ToBuiltin(failure));
  }

 private:
  static constexpr int ToBuiltin(std::memory_order order) noexcept {
    switch (order) {
      case std::memory_order_relaxed:
        return __ATOMIC_RELAXED;
      case std::memory_order_consume:
        return __ATOMIC_CONSUME;
      case std::memory_order_acquire:
        return __ATOMIC_ACQUIRE;
      case std::memory_order_release:
        return __ATOMIC_RELEASE;
      case std::memory_order_acq_rel:
        return __ATOMIC_ACQ_REL;
      default:
        return __ATOMIC_SEQ_CST;
    }
  }

  T* ptr_;
};

#endif

}  // namespace rex
