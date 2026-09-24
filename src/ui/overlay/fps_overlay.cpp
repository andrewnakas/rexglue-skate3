/**
 * @file        ui/overlay/fps_overlay.cpp
 *
 * @brief       Minimal guest FPS readout overlay. See fps_overlay.h for
 *              details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/fps_overlay.h>

#include <algorithm>

#include <imgui.h>
#include <rex/cvar.h>

// On by default, and the default is load-bearing rather than a preference.
//
// This ships on because the average is exactly the statistic that hides a
// stutter - a steady 60 and a 60 that drops four frames a second read the
// same - so anyone who turns the counter on to chase a number wants these.
// It costs nothing until then: the overlay dialog only exists while
// show_fps_counter is set, and these are two lines inside it.
//
// It must be the REGISTERED DEFAULT and not a compiled-in --flag. SaveConfigValues
// erases any key sitting at its registered default (see the comment there - that
// erasure is deliberate, and it is what makes a reset stick). Shipping this on via
// an argument with the default still false meant the player's "Off" equalled the
// default, was erased from settings.toml rather than written to it, and so
// ErasePlayerOwnedArgs could not see that the player had chosen anything - the
// argument came back at the next launch and the row flipped itself on again.
// With the default true, "Off" is the non-default value, gets written, and sticks.
REXCVAR_DEFINE_BOOL(show_fps_percentiles, true, "UI",
                    "Add 1% low FPS and p95/p99 frame times to the FPS counter")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui {

void FpsOverlayDialog::OnDraw(ImGuiIO& io) {
  Presenter::GuestFrameStats stats;
  if (presenter_) {
    stats = presenter_->GetGuestFrameStats();
  }

  constexpr float kMargin = 10.0f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, kMargin), ImGuiCond_Always,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.4f);
  ImGui::PushFont(nullptr, 20.0f);
  if (ImGui::Begin("##fps_overlay", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    if (stats.frame_count > 0 && stats.fps > 0.0) {
      ImGui::Text("%.0f FPS", stats.fps);
      ImGui::PushFont(nullptr, 14.0f);
      ImGui::Text("%.2f ms", stats.frame_time_ms);
      // p99 is the gate rather than p95: both are zero until the ring has two
      // frames inside the four-second window, and p99 is the last to fill.
      if (REXCVAR_GET(show_fps_percentiles) && stats.p99_ms > 0.0) {
        ImGui::Text("1%% low %.0f FPS", stats.low_1pct_fps);
        ImGui::Text("p95 %.2f  p99 %.2f ms", stats.p95_ms, stats.p99_ms);
      }
      // Time the GPU emulation thread spent blocked on host GPU fences last
      // frame: near the frame time = GPU-bound, near zero = thread-bound.
      ImGui::Text("wait %.2f ms", stats.wait_ms);
      if (stats.gpu_ms > 0.0) {
        // Device-timeline span of a recent frame (first to last command,
        // including idle gaps between submissions).
        ImGui::Text("gpu %.2f ms", stats.gpu_ms);
        double gpu_known_ms = stats.gpu_draw_ms + stats.gpu_resolve_ms + stats.gpu_dump_ms;
        if (gpu_known_ms > 0.0) {
          // "other" covers everything not in a bucket: render target
          // ownership transfers, barriers, and idle gaps.
          ImGui::Text("draw %.2f  res %.2f", stats.gpu_draw_ms, stats.gpu_resolve_ms);
          ImGui::Text("dump %.2f  other %.2f", stats.gpu_dump_ms,
                      std::max(0.0, stats.gpu_ms - gpu_known_ms));
        }
      }
      ImGui::PopFont();
    } else {
      ImGui::TextUnformatted("-- FPS");
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

}  // namespace rex::ui
