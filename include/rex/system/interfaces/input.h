/**
 * @file        system/interfaces/input.h
 * @brief       Abstract input system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/system/xtypes.h>

namespace rex::system {

class IInputSystem {
 public:
  virtual ~IInputSystem() = default;
  virtual X_STATUS Setup() = 0;
  virtual void Shutdown() = 0;
  // The machine has woken from a suspend; re-establish any devices that went
  // away while it was asleep. Default is to do nothing, for systems whose
  // devices cannot.
  virtual void OnSystemResume() {}
};

}  // namespace rex::system
