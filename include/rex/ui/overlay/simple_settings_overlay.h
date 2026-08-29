/**
 * @file        rex/ui/overlay/simple_settings_overlay.h
 *
 * @brief       Curated user-facing settings overlay.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rex/ui/graphics_device_list.h>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

void EnsureSimpleSettingsConfig(const std::filesystem::path& config_path);
void SaveSimpleSettingsConfig(const std::filesystem::path& config_path);

struct SimpleProfileInfo {
  std::string id;
  std::string gamertag;
  bool signed_in = true;
};

struct SimpleProfileState {
  std::vector<SimpleProfileInfo> profiles;
  int selected_index = 0;
};

// Live frame timings for the Performance page, sampled from the presenter by
// the host every drawn frame. All zero when the host reports nothing, which
// is what the page shows as "--".
struct SimpleSettingsPerfStats {
  bool valid = false;
  double fps = 0.0;
  double frame_time_ms = 0.0;
  // Time the guest output producer spent blocked on host GPU fences. Near
  // frame_time_ms means the host GPU is the limit; near zero means the
  // producer thread is.
  double wait_ms = 0.0;
  // Host GPU span of the measured frame, and its breakdown. Zero when the
  // backend does not measure them.
  double gpu_ms = 0.0;
  double gpu_draw_ms = 0.0;
  double gpu_resolve_ms = 0.0;
  double gpu_dump_ms = 0.0;
};

// Raw pad snapshot for overlay navigation (host-side, already merged across
// pads). Poll callback runs on the UI thread every drawn frame.
struct SimpleSettingsGamepad {
  bool connected = false;
  uint16_t buttons = 0;  // X_INPUT_GAMEPAD_* bits
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
};

class SimpleSettingsDialog final : public ImGuiDialog {
 public:
  using LoadProfilesCallback = std::function<SimpleProfileState()>;
  using SaveProfileCallback =
      std::function<void(int selected_index, std::string gamertag, bool signed_in)>;
  using CloseSettingsCallback = std::function<void()>;
  using CloseGameCallback = std::function<void()>;
  using RestartGameCallback = std::function<void()>;
  using PollGamepadCallback = std::function<SimpleSettingsGamepad()>;
  using PollPerfStatsCallback = std::function<SimpleSettingsPerfStats()>;

  SimpleSettingsDialog(ImGuiDrawer* drawer, std::filesystem::path config_path,
                       LoadProfilesCallback load_profiles, SaveProfileCallback save_profile,
                       CloseSettingsCallback close_settings, CloseGameCallback close_game,
                       RestartGameCallback restart_game,
                       PollGamepadCallback poll_gamepad = nullptr,
                       PollPerfStatsCallback poll_perf_stats = nullptr);
  ~SimpleSettingsDialog();

  void Show();
  // Opens straight onto the Performance page with the rows focused, which is
  // what the performance chord binds to - landing on the category rail would
  // cost an extra press to get to the thing that was asked for.
  void ShowPerformance();
  void Toggle();
  // Toggles the Performance page: opens there, and closes if it is already
  // the page on screen. Opening the settings on some other page and then
  // hitting the performance chord switches to Performance rather than
  // closing, which is the reading that never loses the user's place.
  void TogglePerformance();
  void Hide();
  // One "back" step (Escape / pad B): text edit -> row focus -> category rail
  // -> closed. The Escape keybind routes here so Escape backs out level by
  // level instead of instantly closing.
  void NavigateBack();
  bool visible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class FocusZone { kRail, kContent };

  struct RowSpec;
  struct NavIntents;

  void LoadSettingsFromCvars();
  bool HasSettingsChanges() const;
  void ReloadProfiles();
  void SaveVideo();
  // Writes a whole quality bundle at once: applies the hot cvars immediately
  // and routes the deferred ones through SaveVideo(), so a preset behaves
  // exactly as if each row had been set by hand.
  void ApplyGraphicsPreset(int preset);
  // Which preset the current settings correspond to, or 0 (Custom).
  int DetectGraphicsPreset() const;
  void SaveProfile();
  void ApplyAndRestart();

  void BuildRows(std::vector<RowSpec>& rows, int category);
  // Rows shared by the Video and Performance pages; each is self-gating and
  // pushes nothing when its cvar is absent. See the .cpp for why they are
  // factored out.
  void PushQualityPresetRow(std::vector<RowSpec>& rows);
  void PushRenderScaleRow(std::vector<RowSpec>& rows);
  void PushFrameCapRow(std::vector<RowSpec>& rows);
  void PushMsaaRow(std::vector<RowSpec>& rows);
  void PushShadowQualityRow(std::vector<RowSpec>& rows);
  void PushSsaoRow(std::vector<RowSpec>& rows);
  void PushBloomRow(std::vector<RowSpec>& rows);
  void PushVolumetricsRow(std::vector<RowSpec>& rows);
  void PushDrawDistanceRow(std::vector<RowSpec>& rows);
  void PushFpsCounterRow(std::vector<RowSpec>& rows);
  // One controller-chord row. `allow_guide` offers the Guide button, which
  // only the level picker can use.
  void PushChordRow(std::vector<RowSpec>& rows, const char* label, const char* desc,
                    const char* cvar_name, int* index, std::string* custom, bool allow_guide);
  NavIntents GatherInput(ImGuiIO& io);

  std::filesystem::path config_path_;
  LoadProfilesCallback load_profiles_;
  SaveProfileCallback save_profile_;
  CloseSettingsCallback close_settings_;
  CloseGameCallback close_game_;
  RestartGameCallback restart_game_;
  PollGamepadCallback poll_gamepad_;
  PollPerfStatsCallback poll_perf_stats_;
  SimpleProfileState profiles_;
  bool visible_ = false;

  // Staged setting values (committed by SaveVideo / SaveProfile).
  GraphicsDeviceList device_list_;
  int graphics_api_index_ = 0;
  int device_index_ = 0;
  // Graphics preset. 0 is "Custom" and is what the row shows whenever the
  // individual settings do not match a preset exactly - selecting a preset
  // writes the settings below, but editing any of them afterwards drops the
  // row back to Custom rather than lying about which preset is active.
  int graphics_preset_index_ = 0;
  int resolution_scale_index_ = 0;
  int frame_cap_index_ = 0;
  int aspect_ratio_index_ = 0;
  int msaa_index_ = 2;
  int shadow_quality_index_ = 2;
  int static_shadow_res_index_ = 2;
  int monitor_index_ = 0;
  int audio_buffer_index_ = 0;
  int language_index_ = 0;
  float field_of_view_ = 60.0f;
  bool fullscreen_ = true;
  bool vsync_ = false;
  bool tearing_ = true;
  bool mnk_mode_ = false;
  bool mnk_capture_mouse_ = false;
  bool profile_signed_in_ = true;
  char gamertag_buf_[32] = {};
  // Live setting values (hot cvars, applied and saved on change).
  bool renderer_native_ = true;
  bool ssao_ = true;
  bool static_shadows_ = true;
  bool shadow_pcss_ = true;
  bool bloom_ = true;
  bool volumetrics_ = true;
  int draw_distance_index_ = 1;
  int stream_probe_index_ = 0;
  bool mode_indicator_ = true;
  bool fps_counter_ = false;
  bool audio_mute_ = false;
  bool rumble_ = true;
  float mnk_sensitivity_ = 1.0f;
  int chord_index_ = 0;
  int perf_chord_index_ = 0;
  int picker_chord_index_ = 0;
  int input_backend_index_ = 0;
  int rumble_scale_index_ = 0;
  int stick_deadzone_index_ = 0;
  int trigger_threshold_index_ = 0;
  bool invert_camera_y_ = false;
  bool swap_sticks_ = false;
  bool guide_button_ = false;
  std::string chord_custom_;
  std::string perf_chord_custom_;
  std::string picker_chord_custom_;
  // Live frame timings, refreshed once per drawn frame while the Performance
  // page is open. Sampling in BuildRows instead would re-poll several times a
  // frame and show different numbers in different rows.
  SimpleSettingsPerfStats perf_stats_;

  // Navigation state.
  FocusZone zone_ = FocusZone::kRail;
  int category_ = 0;
  int rail_sel_ = 0;  // 0..category count-1 = categories; count = the Close Game item
  int row_index_ = 0;
  bool editing_text_ = false;
  bool text_edit_focus_pending_ = false;
  bool pad_active_ = false;      // last nav input came from a pad -> pad legend
  uint16_t prev_pad_buttons_ = 0;
  int held_dir_x_ = 0;           // current held direction (-1/0/1), for repeat
  int held_dir_y_ = 0;
  float repeat_timer_x_ = 0.0f;
  float repeat_timer_y_ = 0.0f;
  float highlight_anim_y_ = -1.0f;  // smoothed selection highlight position
  float rail_anim_y_ = -1.0f;
  float content_scroll_ = 0.0f;       // scroll TARGET, locked to row boundaries
  float content_scroll_anim_ = -1.0f;  // drawn scroll, chases the target
  float wheel_accum_ = 0.0f;           // fractional wheel deltas -> whole notches
  // Drag-to-scroll. The content column could only be scrolled by moving the
  // selection or by a wheel, and a touchscreen has neither - so on iOS every
  // row below the fold was simply unreachable, including the FPS counter on
  // the Performance page. A press that travels turns into a scroll; one that
  // does not is a click, which is why activation waits for the release.
  bool press_dragged_ = false;      // this press has moved past the threshold
  bool drag_in_content_ = false;    // ...and it started inside the content column
  float press_start_x_ = 0.0f;
  float press_start_y_ = 0.0f;
  float drag_start_scroll_ = 0.0f;  // content_scroll_ when the press began
  // Staged video/quality edits are written shortly after they settle, so a hang
  // or a force-quit with the menu open cannot lose them. Hide() flushes too.
  bool video_dirty_ = false;
  float video_dirty_age_ = 0.0f;
  // Swallow the first frame's cursor delta after Show: the pre-open cursor
  // position (or a cursor-mode warp) otherwise reads as mouse motion and
  // steals focus from the rail to whatever row it lands on.
  bool just_shown_ = false;
  float mouse_x_ = -1.0f;        // last mouse position, to detect real motion
  float mouse_y_ = -1.0f;
};

}  // namespace rex::ui
