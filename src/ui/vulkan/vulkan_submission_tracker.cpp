/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/submission_tracker.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_INT32(
    vulkan_fence_wait_timeout_seconds, 4, "UI/Vulkan",
    "Give up waiting for a submission's fence after this many seconds instead of waiting "
    "forever. On Apple platforms a submission can park inside vkQueueSubmit on a "
    "CAMetalDrawable that never arrives - while the app is suspended or its layer is not "
    "visible - and the fence then never signals, wedging the whole process. Giving up turns "
    "that into a dropped frame and a swapchain rebuild. Must stay under iOS's ~5s "
    "suspension deadline, or the system kills the app for failing to go quiescent "
    "before this ever gives up. 0 waits forever (the old behaviour).")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex {
namespace ui {
namespace vulkan {

VulkanSubmissionTracker::FenceAcquisition::~FenceAcquisition() {
  if (!submission_tracker_) {
    // Dropped submission or left after std::move.
    return;
  }
  assert_true(submission_tracker_->fence_acquired_ == fence_);
  if (fence_ != VK_NULL_HANDLE) {
    if (signal_failed_) {
      // Left in the unsignaled state.
      submission_tracker_->fences_reclaimed_.push_back(fence_);
    } else {
      // Left in the pending state.
      submission_tracker_->fences_pending_.emplace_back(submission_tracker_->submission_current_,
                                                        fence_);
    }
    submission_tracker_->fence_acquired_ = VK_NULL_HANDLE;
  }
  ++submission_tracker_->submission_current_;
}

void VulkanSubmissionTracker::Shutdown() {
  // Bounded like every other wait now, so quitting a wedged app does not hang
  // on the way out. The fences below are then destroyed while possibly still
  // in use, which is invalid in principle - but this is teardown, the device
  // goes with it, and a quit that never returns is worse.
  if (!AwaitAllSubmissionsCompletion()) {
    REXLOG_WARN(
        "vulkan: shutting down with submissions still outstanding - their fences never "
        "signalled. Destroying them anyway; the device is going away with them.");
  }
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  for (VkFence fence : fences_reclaimed_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  fences_reclaimed_.clear();
  for (const std::pair<uint64_t, VkFence>& fence_pair : fences_pending_) {
    dfn.vkDestroyFence(device, fence_pair.second, nullptr);
  }
  fences_pending_.clear();
  assert_true(fence_acquired_ == VK_NULL_HANDLE);
  util::DestroyAndNullHandle(dfn.vkDestroyFence, device, fence_acquired_);
}

void VulkanSubmissionTracker::FenceAcquisition::SubmissionFailedOrDropped() {
  if (!submission_tracker_) {
    return;
  }
  assert_true(submission_tracker_->fence_acquired_ == fence_);
  if (fence_ != VK_NULL_HANDLE) {
    submission_tracker_->fences_reclaimed_.push_back(fence_);
  }
  submission_tracker_->fence_acquired_ = VK_NULL_HANDLE;
  fence_ = VK_NULL_HANDLE;
  // No submission acquisition from now on, don't increment the current
  // submission index as well.
  submission_tracker_ = VK_NULL_HANDLE;
}

uint64_t VulkanSubmissionTracker::UpdateAndGetCompletedSubmission() {
  if (!fences_pending_.empty()) {
    const VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    while (!fences_pending_.empty()) {
      const std::pair<uint64_t, VkFence>& pending_pair = fences_pending_.front();
      assert_true(pending_pair.first > submission_completed_on_gpu_);
      if (dfn.vkGetFenceStatus(device, pending_pair.second) != VK_SUCCESS) {
        break;
      }
      fences_reclaimed_.push_back(pending_pair.second);
      submission_completed_on_gpu_ = pending_pair.first;
      fences_pending_.pop_front();
    }
  }
  return submission_completed_on_gpu_;
}

bool VulkanSubmissionTracker::AwaitSubmissionCompletion(uint64_t submission_index) {
  // Diagnostic: a call that takes real time is the whole story of the hang, and
  // without this there is no way to tell "the bound never fired" from "the
  // bound was never reached". Rate-limited to one line a second so a wedged
  // app is legible rather than a wall of text. Costs a steady_clock read per
  // call on the healthy path.
  const auto await_entry_time = std::chrono::steady_clock::now();
  struct AwaitTrace {
    VulkanSubmissionTracker* tracker;
    uint64_t index;
    std::chrono::steady_clock::time_point entry;
    bool* result_slot;
    bool result = false;
    ~AwaitTrace() {
      const auto elapsed = std::chrono::steady_clock::now() - entry;
      if (elapsed < std::chrono::milliseconds(100)) {
        return;
      }
      static std::chrono::steady_clock::time_point s_last_logged;
      const auto now = std::chrono::steady_clock::now();
      if (now - s_last_logged < std::chrono::seconds(1)) {
        return;
      }
      s_last_logged = now;
      REXLOG_WARN(
          "vulkan: AwaitSubmissionCompletion({}) took {}ms -> {} | current={} completed={} "
          "pending={}",
          index, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
          result ? "true" : "false", tracker->submission_current_,
          tracker->submission_completed_on_gpu_, tracker->fences_pending_.size());
    }
  };
  bool await_result_slot = false;
  AwaitTrace await_trace{this, submission_index, await_entry_time, &await_result_slot};
  // The tracker itself can't give a submission index for a submission that
  // hasn't even started being recorded yet, the client has provided a
  // completely invalid value or has done overly optimistic math if such an
  // index has been obtained somehow.
  assert_true(submission_index <= submission_current_);
  // Waiting for the current submission is fine if there was a failure or a
  // refusal to submit, and the submission index wasn't incremented, but still
  // need to release objects referenced in the dropped submission (while
  // shutting down, for instance - in this case, waiting for the last successful
  // submission, which could have also referenced the objects from the new
  // submission - we can't know since the client has already overwritten its
  // last usage index, would correctly ensure that GPU usage of the objects is
  // not pending). Waiting for successful submissions, but failed signals, will
  // result in a true race condition, however, but waiting for the closest
  // successful signal is the best approximation - also retrying to signal in
  // this case.
  // Go from the most recent to wait only for one fence, which includes all the
  // preceding ones.
  // "Fence signal operations that are defined by vkQueueSubmit additionally
  // include in the first synchronization scope all commands that occur earlier
  // in submission order."
  size_t reclaim_end = fences_pending_.size();
  if (reclaim_end) {
    const VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    while (reclaim_end) {
      const std::pair<uint64_t, VkFence>& pending_pair = fences_pending_[reclaim_end - 1];
      assert_true(pending_pair.first > submission_completed_on_gpu_);
      if (pending_pair.first <= submission_index) {
        // Wait if requested.
        //
        // Bounded and retried rather than a single UINT64_MAX wait. The
        // semantics are identical - this still does not return until the fence
        // signals - but an infinite wait here is invisible, and it is a real
        // failure mode on iOS: the system can suspend the process mid-frame,
        // and a submission parked on a CAMetalDrawable that never arrives
        // leaves this blocked forever on return, with no log line and nothing
        // for the hang watchdog to attribute. Saying so turns "it hung coming
        // back from the home screen" into a stack.
        // Bound this OURSELVES rather than trusting vkWaitForFences.
        //
        // Measured on device 2026-08-27: a 2s timeout passed to MoltenVK's
        // vkWaitForFences did NOT come back as VK_TIMEOUT. The main thread sat
        // in it for 17s while the app was wedged, and the per-slice warning
        // below never printed once - the cvar was confirmed applied, so the
        // code was live and the timeout simply was not honoured. MoltenVK
        // implements the wait as condition_variable::wait_until against a
        // system_clock deadline, so a Vulkan-level timeout is only as good as
        // that conversion. It is not something to build a hang bound on.
        //
        // vkGetFenceStatus does not block, so polling it against our own
        // steady_clock deadline cannot be defeated the same way. The spin is
        // short and yields, then backs off to a sleep, so a healthy frame pays
        // microseconds and a wedged one is bounded for real.
        const int32_t timeout_s = REXCVAR_GET(vulkan_fence_wait_timeout_seconds);
        const auto poll_start = std::chrono::steady_clock::now();
        const auto deadline = poll_start + std::chrono::seconds(timeout_s > 0 ? timeout_s : 0);
        uint32_t next_warn_s = 2;
        bool gave_up = false;
        VkResult wait_result;
        for (;;) {
          wait_result = dfn.vkGetFenceStatus(device, pending_pair.second);
          if (wait_result != VK_NOT_READY) {
            break;
          }
          const auto now = std::chrono::steady_clock::now();
          const auto waited = now - poll_start;
          if (timeout_s > 0 && now >= deadline) {
            REXLOG_ERROR(
                "vulkan: giving up waiting for submission {} after {}s. The fence is still "
                "unsignalled, so the GPU may still own everything this submission referenced: "
                "the fence is NOT recycled and the submission is NOT retired here, and the "
                "caller drops the frame rather than reusing the slot. Waiting forever instead "
                "would wedge the process, which on iOS is a kill for failing to suspend.",
                pending_pair.first,
                std::chrono::duration_cast<std::chrono::seconds>(waited).count());
            gave_up = true;
            break;
          }
          if (waited >= std::chrono::seconds(next_warn_s)) {
            REXLOG_WARN(
                "vulkan: still waiting for submission {} to complete after {}s - the GPU has "
                "not signalled its fence. On Apple platforms this is what a drawable that never "
                "arrives looks like (the app was most likely suspended mid-frame).",
                pending_pair.first, next_warn_s);
            next_warn_s += 2;
          }
          // Cheap for the first couple of ms - a fence about to signal usually
          // does so well inside that - then back off so a long wait is idle.
          if (waited < std::chrono::milliseconds(2)) {
            std::this_thread::yield();
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
        if (gave_up) {
          await_trace.result = false;
          // Leave the pending entry exactly as it is. It is still a live fence
          // the GPU may signal late - the logs show these do sometimes come
          // back - and a later call will pick it up through the ordinary
          // vkGetFenceStatus path. Reclaiming or resetting it here is how a
          // bounded wait would turn a hang into memory corruption.
          return false;
        }
        if (wait_result == VK_SUCCESS) {
          break;
        }
      }
      // Just refresh the completed submission.
      if (dfn.vkGetFenceStatus(device, pending_pair.second) == VK_SUCCESS) {
        break;
      }
      --reclaim_end;
    }
    if (reclaim_end) {
      submission_completed_on_gpu_ = fences_pending_[reclaim_end - 1].first;
      for (; reclaim_end; --reclaim_end) {
        fences_reclaimed_.push_back(fences_pending_.front().second);
        fences_pending_.pop_front();
      }
    }
  }
  // >= not ==: the caller is asking "has this submission completed", and the
  // GPU having run PAST it is the ordinary healthy answer, not a failure. With
  // == this returned false on every frame where the completed index had moved
  // on, which is most of them - callers that treat false as "the fence never
  // signalled" then drop every frame.
  await_trace.result = submission_completed_on_gpu_ >= submission_index;
  return await_trace.result;
}

VulkanSubmissionTracker::FenceAcquisition
VulkanSubmissionTracker::AcquireFenceToAdvanceSubmission() {
  assert_true(fence_acquired_ == VK_NULL_HANDLE);
  // Reclaim fences if the client only gets the completed submission index or
  // awaits in special cases such as shutdown.
  UpdateAndGetCompletedSubmission();
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (!fences_reclaimed_.empty()) {
    VkFence reclaimed_fence = fences_reclaimed_.back();
    if (dfn.vkResetFences(device, 1, &reclaimed_fence) == VK_SUCCESS) {
      fence_acquired_ = fences_reclaimed_.back();
      fences_reclaimed_.pop_back();
    }
  }
  if (fence_acquired_ == VK_NULL_HANDLE) {
    VkFenceCreateInfo fence_create_info;
    fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_create_info.pNext = nullptr;
    fence_create_info.flags = 0;
    // May fail, a null fence is handled in FenceAcquisition.
    dfn.vkCreateFence(device, &fence_create_info, nullptr, &fence_acquired_);
  }
  return FenceAcquisition(*this, fence_acquired_);
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
