#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include <SDL3/SDL.h>

#if REX_PLATFORM_IOS
#include <os/proc.h>  // os_proc_available_memory, for PickStoreBudgets

REXCVAR_DEFINE_BOOL(vulkan_mvk_present_with_command_buffer, false, "GPU/Vulkan",
                    "Have MoltenVK present through a command buffer instead of calling "
                    "presentDrawable on the drawable itself. A different route to the same "
                    "result, and it moves where the presentation completion block is created.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_mvk_synchronous_queue_submits, true, "GPU/Vulkan",
                    "Encode Metal command buffers on the thread that submits them. On means the "
                    "encode is a known cost on the critical path; off moves it to MoltenVK's own "
                    "queue thread, which also moves the blocking wait for a drawable off the "
                    "submitting thread - see the note at the setenv call.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(vulkan_mvk_log_level, 1, "GPU/Vulkan",
                     "MoltenVK log level: 0 none, 1 errors, 2 warnings, 3 info. Above 1 is "
                     "expensive - every line is a synchronous write to flash on whichever "
                     "thread logged it.")
    .range(0, 4)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif
#if REX_PLATFORM_ANDROID
// Renames main() to SDL_main, which is what SDLActivity's native loader calls.
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <rex/main_android.h>
#endif

#if REX_PLATFORM_MOBILE
// SDL's iOS entry point also goes through SDL_main.
#include <SDL3/SDL_main.h>
#if REX_PLATFORM_IOS
#include <CoreFoundation/CoreFoundation.h>
#endif
#if REX_PLATFORM_ANDROID
#include <sys/sysinfo.h>
#include <system_error>
#endif
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace {

// iOS launches an app with no arguments, and there is no Java shell to build
// them the way Skate3Activity does on Android, so the paths are derived here.
// getenv("HOME") is the app sandbox root under iOS, making $HOME/Documents the
// user-visible directory that files copied in via Finder or iTunes land in.
// Arguments set here beat settings.toml, which is what makes them reliable and
// also what makes them impossible to tune without a 30-minute rebuild-and-sign
// cycle. `Documents/user/ios_args.txt` closes that gap: one argument per line,
// blank lines and `#` comments ignored, and any `--key=` it names replaces the
// built-in entry for that key rather than sitting alongside it. Nothing is
// required to be in the file, and a file that is not there costs nothing.
std::vector<std::string> ApplyArgumentFileOverrides(std::vector<std::string> args,
                                                    const std::filesystem::path& override_file,
                                                    const char* tag) {
  std::ifstream in(override_file);
  if (!in) {
    return args;
  }

  std::vector<std::string> overrides;
  std::string line;
  while (std::getline(in, line)) {
    // Trim both ends; a stray \r from a file edited on a desktop would
    // otherwise become part of the value.
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      continue;
    }
    const auto last = line.find_last_not_of(" \t\r\n");
    line = line.substr(first, last - first + 1);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.rfind("--", 0) != 0) {
      line = "--" + line;
    }
    overrides.push_back(std::move(line));
  }
  if (overrides.empty()) {
    return args;
  }

  auto key_of = [](const std::string& arg) {
    const auto eq = arg.find('=');
    return eq == std::string::npos ? arg : arg.substr(0, eq);
  };

  // Collapse repeats WITHIN the file first, last line winning. CLI11 rejects a
  // scalar option it sees twice, and that rejection is not local: the whole
  // parse fails, so every compiled-in argument below is silently discarded and
  // the app comes up on stock defaults. One duplicated line in this file is
  // therefore enough to quietly undo all of the tuning it exists to carry -
  // which is exactly what happened, and it cost an evening to find because the
  // only evidence is one line in stderr.log.
  {
    std::vector<std::string> deduped;
    deduped.reserve(overrides.size());
    for (auto it = overrides.rbegin(); it != overrides.rend(); ++it) {
      const std::string key = key_of(*it);
      const bool already =
          std::any_of(deduped.begin(), deduped.end(),
                      [&](const std::string& kept) { return key_of(kept) == key; });
      if (!already) {
        deduped.push_back(*it);
      } else {
        std::fprintf(stderr, "%s: ignoring an earlier duplicate of %s\n", tag, key.c_str());
      }
    }
    std::reverse(deduped.begin(), deduped.end());
    overrides = std::move(deduped);
  }

  for (const std::string& override_arg : overrides) {
    const std::string key = key_of(override_arg);
    args.erase(std::remove_if(args.begin(), args.end(),
                              [&](const std::string& arg) { return key_of(arg) == key; }),
               args.end());
  }
  args.insert(args.end(), overrides.begin(), overrides.end());

  // These, and not the compiled-in arguments above them, are the operator's
  // intent on this platform: BuildIOSArguments is a set of shipped defaults
  // that reaches cvar::Init through the same argv. Anything applying a bundle
  // of settings later - a video preset - has to leave these alone.
  for (const std::string& override_arg : overrides) {
    // key_of keeps the "--" it added above; the registry holds bare names.
    rex::cvar::NoteExplicitlySet(std::string_view(key_of(override_arg)).substr(2));
  }

  // Say so. An override that silently fails to apply is indistinguishable from
  // one that applied and did nothing, and telling those apart by watching frame
  // times is exactly the guessing this file exists to avoid. Logging is not up
  // yet at this point - these arguments are what configures it - so this goes
  // to stderr, which iOS has already been pointed at Documents/stderr.log.
  std::fprintf(stderr, "%s: applied %zu override(s) from %s\n", tag, overrides.size(),
               override_file.c_str());
  for (const std::string& override_arg : overrides) {
    std::fprintf(stderr, "%s:   %s\n", tag, override_arg.c_str());
  }
  std::fflush(stderr);

  return args;
}

#if REX_PLATFORM_IOS
// Texture / mesh store budgets, chosen from the memory this process is
// actually allowed rather than from a constant.
//
// The large tier IS v2.5.0: 288 MB of textures, 224 MB of meshes, the values
// every 4 GB phone shipped with and the only configuration this build has been
// played against with the retired-object drain in its shipped (unbounded)
// form. A 448/128 rebalance was tried on 2026-09-01 and measured fewer
// eviction sweeps (6 vs 50 in comparable play), but every one of those runs
// also carried a capped catch-up drain that left 11-17k retired objects
// walked by erase_if on EVERY frame - the game was broadly slower and the
// budget half of the change could not be separated from the drain half. Until
// 448 is measured on its own, against the unbounded drain, the tested values
// stay. Both budgets remain overridable from Documents/user/ios_args.txt with
// no rebuild (skate3_native_render_scene_tex_store_mb / _mesh_store_mb), which
// is how the next measurement should be taken.
//
// A FLOOR OF 256 MB APPLIES TO BOTH STORES and is not visible from here:
// skate3_native_scene_gpu.cpp computes each cap as
// `std::max(256, <the cvar>) << 20`, so any value below 256 is silently
// raised. That is why every "mesh store LRU start" line ever logged says
// cap_mb=256 - at 224 (v2.5.0), and at the 128 the 2.6.0 candidate briefly
// set. The mesh half of that rebalance was a no-op, which is also why it
// could not have been the gameplay regression it was blamed for. The mesh
// numbers below are therefore documentation of intent, not of effect, and
// the medium and small tiers cannot lower either store at all until that
// floor is raised or removed. Leave them honest rather than reassuring.
//
// The lower tiers exist because 288/224 is only proven on a 4 GB phone.
// Supported 6-core iPhones include 3 GB parts (XR, SE 2020) that were never
// tested here, and on iOS the penalty for guessing high is not a slow frame,
// it is jetsam killing the app. os_proc_available_memory() is the honest
// input: it reports what is left before that happens, it is the same number
// the periodic "ios mem ... headroom" line prints, and it is read here before
// the guest allocates anything, so it is close to the whole allowance (2342 MB
// on a 4 GB 13 mini, verified from stderr). The mesh store never exceeded
// 97 MB in any log, so the smaller tiers cut textures first.
struct StoreBudgets {
  uint32_t tex_mb;
  uint32_t mesh_mb;
  const char* tier;
};

StoreBudgets PickStoreBudgets() {
  const uint64_t avail_mb = uint64_t(os_proc_available_memory()) >> 20;
  StoreBudgets b;
  if (avail_mb == 0) {
    // The API answered nothing (it returns 0 outside a memory-limited
    // process). Take the shipped values rather than a number no device has run.
    b = {288, 224, "unknown-availability"};
  } else if (avail_mb >= 2000) {
    // A 4 GB phone reports ~2342 MB at startup. The threshold sits well below
    // that so a 4 GB device can never land in the medium tier by accident.
    b = {288, 224, "large"};
  } else if (avail_mb >= 1400) {
    b = {256, 192, "medium"};
  } else {
    b = {224, 160, "small"};
  }
  std::fprintf(stderr, "store budgets: %s tier (%llu MB allowable) -> tex %u MB, mesh %u MB\n",
               b.tier, (unsigned long long)avail_mb, b.tex_mb, b.mesh_mb);
  std::fflush(stderr);
  return b;
}

std::vector<std::string> BuildIOSArguments() {
  const StoreBudgets store_budgets = PickStoreBudgets();
  const char* home = std::getenv("HOME");
  const std::filesystem::path documents =
      std::filesystem::path(home ? home : ".") / "Documents";
  std::vector<std::string> args = {
      "--game_data_root=" + (documents / "game").string(),
      "--user_data_root=" + (documents / "user").string(),
      "--log_file=" + (documents / "skate3.log").string(),
      "--log_flush_interval=1",
      // Every flush is a synchronous write to flash on whichever thread logged,
      // and the command-processor and render threads log routinely. Flush on
      // trouble; the one-second interval above carries everything else, so a
      // crash still loses at most a second of history.
      "--log_flush_level=warn",
      // The emulated Xenos backend needs geometry shaders, which Metal (and so
      // MoltenVK) does not expose; the Skate-3 native renderer replaces it.
      "--skate3_native_render_scene=true",
      "--vulkan_require_geometry_shader=false",
      "--vulkan_require_fill_mode_non_solid=false",
      // Left off deliberately. The messenger mirrors MoltenVK's own reports
      // into the log at whatever severity the gpu category is set to, and the
      // logger flushes to flash inline on the thread that logged - so on a
      // chatty frame this bills the render path for a synchronous write per
      // message. Turn it back on when debugging the renderer, not to play.
      "--vulkan_log_debug_messages=false",
      // Content packs are dropped into Documents like the disc is, and
      // installing them is how a custom map reaches the game's own Freeskate
      // list. This used to be forced off to keep the install WIZARD from
      // running; the wizard is a separate thing and the packs are worth having.
      "--skate3_auto_install_dlc=true",
      // Present through a command buffer rather than calling presentDrawable
      // on the drawable itself. Sessions were being ended by a fault inside
      // MoltenVK's own presentCAMetalDrawable - in the completion block it
      // hands to Metal - which survived every fix aimed at our own memory,
      // the swapchain lifetime and the system's low-memory warning, and which
      // MoltenVK's latest release still has. This route builds that completion
      // somewhere else and does not fault. The alternative that also worked
      // was synchronous submits, which costs half the frame rate.
      "--vulkan_mvk_present_with_command_buffer=true",
      // EXACTLY DOUBLE THE FRAME RATE. Measured on an iPhone 13 mini,
      // 2026-08-28, same device, same scene, this line the only difference:
      //
      //   sync  (the old compiled default)   fps 30.5-31.0  p50 33.3ms
      //   async (this)                       fps 58.6-59.6  p50 16.7ms
      //   avg_submit_us                      16200  ->  2
      //
      // MoltenVK encodes a VkCommandBuffer into a MTLCommandBuffer inline
      // inside vkQueueSubmit, and that encode costs a whole frame. Synchronous
      // submits run it on the calling thread, so the paint thread spends the
      // entire 16.7 ms budget inside the submit and can only ever produce every
      // second vblank. Moving it off-thread makes the submit free.
      //
      // The comment further down this file argues the opposite, having measured
      // async as WORSE for frame-to-frame consistency (a device-level lock held
      // across the encode stalling resource creation). That is not what this
      // measurement shows - async was also steadier, sd 0.53-1.9 ms against
      // 3.5-65 ms. Note the earlier finding was made at a time when this was
      // being set from device-args/ios_args.txt, which is why the dev phone has
      // carried `=false` for months while every shipped build ran `=true`: the
      // 13 mini's "locked 60" was measured on a configuration no player had.
      // Retest under heavy world streaming before treating the consistency
      // question as settled - that is where the lock contention would show, and
      // this measurement was taken at a menu.
      "--vulkan_mvk_synchronous_queue_submits=false",
      // Per-window frame breakdown. The formatted line is genuinely cheap -
      // one every 600 guest frames - but the accounting behind it is not, and
      // that part ran whether or not the line was ever printed. Off by default
      // now; skate3_diagnostics turns it back on, and the Diagnostics switch on
      // the System page turns THAT on without a rebuild, which is what this was
      // compiled in to achieve in the first place.
      "--skate3_native_render_scene_perf_log=false",
      // Mitigation for the WorldPresentation cross-thread use-after-free.
      "--skate3_instance_free_defer_ms=250",

      // ---- Frame pacing ---------------------------------------------------
      // 30, an exact 2:1 cadence on the 60 Hz panel: every frame shown for
      // exactly two refreshes, which reads as locked where an unstable 40 reads
      // as judder.
      //
      // This was briefly 60, and the frame time supports it - with the system
      // command buffer fence acknowledged rather than timed out the median
      // frame is 16.7ms, so the panel rate is genuinely reachable. What is not
      // reachable is holding it: measured on device, an uncapped run starts at
      // 57 fps and decays - 54, then 29, then 4.8, then 0.2 - while the kernel
      // begins killing idle daemons and free memory falls to around 38 MB.
      // 60 as of the residency + GPU work this comment used to wait on:
      // suppress_mode=1 took GPU time from 17.8-19.4 ms to 11.0-11.9 ms, the
      // occlusion cull now runs on iOS at all (its depth grid had only ever
      // been produced inside the SSAO pass, which iOS disables), and the cache
      // budgets below hold the resident set near 1 GB instead of 1.4 GB.
      // Measured locked 60 on an iPhone 13 mini: p50 16.7 ms, p95 16.8-17.8 ms.
      // Drop to 30 from Documents/user/ios_args.txt on a smaller device.
      "--skate3_guest_fps_cap=60",
      "--skate3_guest_fps_cap_auto=false",
      // Auto is still reachable from the settings menu, and on a ProMotion
      // phone it used to derive 114 FPS from a 120 Hz panel that iOS only
      // presents 60 through - which stopped the pacer pacing and locked the
      // frame to 30. Fixed at the source (display_presentable_refresh_cap_hz),
      // but the next report of this shape should carry the evidence rather
      // than needing a Metal HUD capture to diagnose, so:
      //
      // The cadence line is avg/sd/jitter/max over 600 swaps - one formatted
      // line every ten seconds or so, which cannot distort what it measures.
      // Together with [pace] and the "guest frame cap is now" line it shows
      // the cap, the rate the display accepts, and what actually came out.
      //
      // The statistics behind it, though, are accumulated on every single
      // guest-output refresh, so leaving this on bills every player for an
      // instrument almost none of them will ever read. Under skate3_diagnostics.
      "--presenter_present_cadence_log=false",
      // Breaks one paint into acquire / record / end-command-buffer / submit /
      // present microseconds, averaged over 120 frames. This is the instrument
      // that says WHERE a frame goes when [pace] reports 33 ms while the GPU
      // span is 4.6 ms and the command processor never waits on a fence - the
      // state an A17 Pro is in as of 2026-08-28. This used to be compiled ON,
      // because the people who can reproduce that are strangers on the internet
      // and one duplicated key in Documents/user/ios_args.txt silently discards
      // every argument the app was built with - so asking them to edit that
      // file was never a plan. The Diagnostics switch on the System page is the
      // answer to it instead: one toggle, no file, no rebuild.
      //
      // Off matters here more than for the others, because the breakdown is ten
      // steady_clock reads per present taken BEFORE anything checks whether the
      // log is on.
      "--vulkan_present_timing_log=false",
      // Warn, not info. [pace] and [cp-sum] are INFO and so are silent by
      // default now, which is the point: a shipped build should not narrate its
      // own frame timing to flash. skate3_diagnostics raises this back to info
      // along with the instruments that write at that level, so the switch
      // produces a usable support log rather than an empty one.
      "--log_level=warn",

      // ---- Cache budgets --------------------------------------------------
      // These default to 1280 and 1024 MB, which is 2.3 GB of caches before
      // either LRU evicts anything. That is a desktop budget: on device the
      // measured growth was ~18 MB a second with neither LRU ever starting,
      // reaching 1382 MB of the 2730 MB heap in about half a minute and dying
      // shortly after - and iOS kills the app well before the heap budget is
      // the binding constraint. Textures cost more here than the numbers
      // suggest, too, since Metal exposes no BC formats and every DXT surface
      // is expanded to RGBA8 on upload (8x for DXT1).
      // Sized against what the KERNEL will actually let this process have - see
      // PickStoreBudgets above. On a 4 GB phone these are v2.5.0's 288/224.
      "--skate3_native_render_scene_tex_store_mb=" + std::to_string(store_budgets.tex_mb),
      "--skate3_native_render_scene_mesh_store_mb=" + std::to_string(store_budgets.mesh_mb),

      // The Xenos texture cache is a SEPARATE budget from the two above, and
      // on device it is the largest single consumer of device-local memory:
      // measured heap0 use=1448MB while the native stores held only 414MB and
      // upload buffers 165MB. Menus and loading always render fully (the
      // native renderer yields there), so it fills to its HARD limit during
      // every map load and then sits there, because gameplay - with the
      // emulated passes suppressed - never touches it again to age it out.
      // The soft limit cannot bound that; only the hard one can. Leaving it at
      // the desktop 2048/4096 ran the process to ~1.4 GB resident, which is
      // where the MoltenVK presentation fault starts firing.
      "--texture_cache_memory_limit_soft=256",
      "--texture_cache_memory_limit_hard=320",
      "--texture_cache_memory_limit_soft_lifetime=30",
      "--texture_cache_memory_limit_render_to_texture=24",

      // Suppress the emulated Xenos passes the native renderer replaces.
      // Mode 2 keeps the memory-composition passes alive; measured with GPU
      // timestamps, what they leave behind costs ~7 ms/frame - render-pass
      // entry alone was 3.2 ms across 10 passes, and on a tile GPU every pass
      // entry is a tile load/store. Total GPU time was 17.8-19.4 ms against a
      // 16.67 ms budget; mode 1 brings it to 11.0-11.9 ms, which is the
      // difference between ~50 fps and a locked 60. Mode 1 is documented to
      // break mid-gameplay lightmap page composition; it was not visible in
      // play testing on an iPhone 13 mini, but set 2 if lighting looks wrong.
      "--native_render_suppress_mode=1",

      // ---- Memory budget -------------------------------------------------
      // A 4 GB iPhone allows roughly 2 GB resident, near 3 GB with the
      // increased-memory-limit entitlement, and jetsam kills rather than
      // swaps. The desktop defaults measured 3.2 GB of render-target memory
      // alone, so the render targets are brought back to native size and the
      // most attachment-hungry effects are off by default here. None of this
      // is a hard limit: every value can be overridden from settings.toml
      // once there is headroom to spend.
      //
      // Scale is the dominant term - both axes at 2 means four times the
      // pixels in every full-screen target.
      "--resolution_scale=1",
      "--draw_resolution_scale_x=1",
      "--draw_resolution_scale_y=1",
      // 4x MSAA quadruples every multisampled attachment.
      "--skate3_native_render_scene_msaa=1",
      // A 4096 static shadow atlas is 64 MB before mips; 1024 is 4 MB.
      "--skate3_native_render_scene_shadow_static_size=1024",
      "--skate3_native_render_scene_shadow_pcss=false",
      // Each of these carries its own full-screen intermediate target.
      "--skate3_native_render_scene_ssao=false",
      "--skate3_native_render_scene_bloom=false",
      "--skate3_native_render_scene_shafts=false",
      // Draw and LOD distance drive how much of the world is resident at
      // once, which is guest-heap pressure rather than GPU memory.
      //
      // Both cvars DEFAULT TO 2.0 - twice what the console drew - and leaving
      // them unset here meant every iOS device ran at twice the draw radius,
      // so roughly four times the world area resident and streaming. That is
      // affordable standing still and is not affordable at speed: crossing
      // streaming cell boundaries fast turns it into whole-second frames, and
      // it is why "it deteriorates once you get going" was reproducible while
      // the median frame stayed a perfect 16.7 ms.
      //
      // 1.0 is the ORIGINAL CONSOLE behavior, not a degradation below it, and
      // it measurably cut the guest render thread from 90-95% of a core to
      // 53-83%. The earlier objection to setting these - that pinning them
      // beats settings.toml, so someone asking for 0.75 silently got more -
      // is answered by Documents/user/ios_args.txt, which overrides anything
      // here without a rebuild.
      "--skate3_draw_distance_scale=1.0",
      "--skate3_lod_distance_scale=1.0",

      // ---- Command processor stalls --------------------------------------
      // Skate 3 parks the command processor on a WAIT_REG_MEM poll that never
      // clears here: the value it waits on is published by guest code that is
      // itself queued behind the stalled processor, and the vblank thread that
      // exists to break that cycle cannot, because the interrupt handler it
      // runs needs a critical section held by one of the blocked threads.
      //
      // The generic timeout is deliberately patient, since abandoning a wait
      // early can render a frame from data that is not ready. That patience is
      // wrong here: the wait is not slow, it is permanently unsatisfied, so
      // every full timeout is dead time. Measured on an iPhone 13 mini, 500 ms
      // left the processor blocked about 91% of the time. A genuine wait
      // clears in microseconds, so 20 ms is still thousands of polls of grace
      // while cutting the cost of the chronic case by 25x.
      "--gpu_wait_reg_mem_timeout_ms=20",

      // ---- Vblank cadence ------------------------------------------------
      // With vsync off the vblank worker does not idle at the display rate, it
      // free-runs at 1000 Hz, and every one of those ticks runs the guest's
      // graphics interrupt handler while holding the global lock. That is ~940
      // dispatches a second of pure contention against the command processor
      // and every guest thread, for a phone that cannot present faster than 60.
      //
      // It also decides how a WAIT_REG_MEM poll waits: with vsync off the poll
      // takes the busy-spin branch and burns a core, with it on the poll sleeps.
      // This lived in settings.toml, where a rewrite of the file could silently
      // drop it - and a command-line argument beats the file anyway.
      "--vsync=true",

      // MoltenVK advertises IMMEDIATE, but a CAMetalLayer on iOS has no
      // equivalent of displaySyncEnabled, so what actually happens is FIFO
      // while the presenter believes it is unsynchronised and paces against a
      // tear that never comes. Ask for what the platform really does.
      "--vulkan_allow_present_mode_immediate=false",
      "--vulkan_allow_present_mode_mailbox=false",

      // ---- Audio -----------------------------------------------------------
      // Both of these are what fixed "the sound is awful" on macOS, which runs
      // byte-identical audio code (the subsystem has no platform conditionals
      // at all), and iOS had neither.
      //
      // SDL clamps the device to at least the channel count asked for and the
      // iOS backend never adjusts it, so "ask the device" always answers 6 here
      // and the 5.1 fold silently goes to CoreAudio - the route
      // sdl_audio_driver.cpp itself calls untested. 2 forces our own downmix.
      "--audio_device_channels=2",

      // 512 frames = 10.7ms. macOS/CoreAudio honours this exactly and it is
      // what made the game sound right there; the default with nothing asked
      // for is 1024. Confirm what iOS actually chose from the
      // "SDLAudioDriver: device ... sample_frames=" line in Documents/skate3.log,
      // and judge the result by ear - frames/s, silence_chunks and queue depth
      // read IDENTICALLY for a good and a bad buffer size, so no counter here
      // can confirm it. Override from Documents/user/ios_args.txt to try other
      // values without a rebuild.
      "--audio_device_sample_frames=512",
  };

  return ApplyArgumentFileOverrides(std::move(args), documents / "user" / "ios_args.txt",
                                    "ios_args");
}
#endif  // REX_PLATFORM_IOS

#if REX_PLATFORM_ANDROID
// Where the app may write. SDL asks the activity for getExternalFilesDir(),
// which is /storage/emulated/0/Android/data/<package>/files: visible to the
// player through any file manager, needs no permission, and is where `adb
// push` lands the game dump during development. HOME is unset in an app
// process and the runtime's fallback user directory is unwritable, so both
// roots MUST be named from here rather than left to the defaults. The Java
// shell also exports SKATE3_EXTERNAL_FILES_DIR in case SDL's JNI lookup answers
// nothing, and the app-private directory is the last resort.
std::filesystem::path AndroidFilesRoot() {
  if (const char* sdl_path = SDL_GetAndroidExternalStoragePath(); sdl_path && *sdl_path) {
    return sdl_path;
  }
  if (const char* env_path = std::getenv("SKATE3_EXTERNAL_FILES_DIR"); env_path && *env_path) {
    return env_path;
  }
  if (const char* internal = SDL_GetAndroidInternalStoragePath(); internal && *internal) {
    return internal;
  }
  return ".";
}

// Texture / mesh store budgets from the device's RAM. 288/224 is the only pair
// this renderer has been played against at length (every 4 GB iPhone shipped
// with it, see PickStoreBudgets in the iOS branch), so an 8 GB phone starts
// there too and raises it from user/android_args.txt once the eviction rate
// has been measured rather than guessed. The 256 MB floor in
// skate3_native_scene_gpu.cpp applies here as well.
struct AndroidStoreBudgets {
  uint32_t tex_mb;
  uint32_t mesh_mb;
  const char* tier;
};

AndroidStoreBudgets PickAndroidStoreBudgets() {
  struct sysinfo info = {};
  const uint64_t total_mb =
      sysinfo(&info) == 0 ? (uint64_t(info.totalram) * uint64_t(info.mem_unit)) >> 20 : 0;
  AndroidStoreBudgets b;
  if (total_mb == 0) {
    b = {288, 224, "unknown"};
  } else if (total_mb >= 5000) {
    b = {288, 224, "standard (6 GB+)"};
  } else {
    b = {224, 160, "small"};
  }
  std::fprintf(stderr, "store budgets: %s tier (%llu MB RAM) -> tex %u MB, mesh %u MB\n", b.tier,
               (unsigned long long)total_mb, b.tex_mb, b.mesh_mb);
  std::fflush(stderr);
  return b;
}

// The shipped Android defaults. Every entry is a real cvar: CLI11 rejects an
// unknown option and a failed parse silently discards the whole list, so
// nothing speculative belongs here. Rationale for each value is in the iOS
// list above; only the MoltenVK-specific knobs are missing, because there is
// no MoltenVK. Anything here can be replaced without a rebuild from
// <files>/user/android_args.txt, one argument per line.
std::vector<std::string> BuildAndroidArguments() {
  const std::filesystem::path root = AndroidFilesRoot();
  const AndroidStoreBudgets store_budgets = PickAndroidStoreBudgets();
  std::vector<std::string> args = {
      "--game_data_root=" + (root / "game").string(),
      "--user_data_root=" + (root / "user").string(),
      "--log_file=" + (root / "skate3.log").string(),
      "--log_flush_interval=1",
      "--log_flush_level=warn",
      "--log_level=warn",
      // The native renderer replaces the emulated Xenos pipeline; it is the
      // whole reason a phone can run this at all.
      "--skate3_native_render_scene=true",
      "--skate3_auto_install_dlc=true",
      "--vulkan_log_debug_messages=false",
      "--skate3_native_render_scene_perf_log=false",
      "--presenter_present_cadence_log=false",
      "--vulkan_present_timing_log=false",
      // Mitigation for the WorldPresentation cross-thread use-after-free.
      "--skate3_instance_free_defer_ms=250",
      // FIFO on a 60 Hz mode (the activity picks it) paces a locked 60; the
      // cap keeps the guest from running ahead of the panel.
      "--skate3_guest_fps_cap=60",
      "--skate3_guest_fps_cap_auto=false",
      "--vsync=true",
      "--skate3_native_render_scene_tex_store_mb=" + std::to_string(store_budgets.tex_mb),
      "--skate3_native_render_scene_mesh_store_mb=" + std::to_string(store_budgets.mesh_mb),
      // The Xenos texture cache fills to its HARD limit on every map load
      // (menus and loading render through the emulated path) and then sits
      // there. 8 GB of RAM buys more headroom than the phone that set 320.
      "--texture_cache_memory_limit_soft=256",
      "--texture_cache_memory_limit_hard=384",
      "--texture_cache_memory_limit_soft_lifetime=30",
      "--texture_cache_memory_limit_render_to_texture=24",
      // Adreno is a tile GPU too: every emulated render-pass entry the native
      // renderer leaves behind is a tile load/store. Mode 1 drops them.
      "--native_render_suppress_mode=1",
      "--resolution_scale=1",
      "--draw_resolution_scale_x=1",
      "--draw_resolution_scale_y=1",
      "--skate3_native_render_scene_msaa=1",
      "--skate3_native_render_scene_shadow_static_size=1024",
      "--skate3_native_render_scene_shadow_pcss=false",
      "--skate3_native_render_scene_ssao=false",
      "--skate3_native_render_scene_bloom=false",
      "--skate3_native_render_scene_shafts=false",
      // The console's own draw radius; the 2.0 default is four times the
      // world area streaming and is what makes fast travel hitch.
      "--skate3_draw_distance_scale=1.0",
      "--skate3_lod_distance_scale=1.0",
      "--gpu_wait_reg_mem_timeout_ms=20",
      "--audio_device_channels=2",
      "--audio_device_sample_frames=512",
  };
  return ApplyArgumentFileOverrides(std::move(args), root / "user" / "android_args.txt",
                                    "android_args");
}
#endif  // REX_PLATFORM_ANDROID

}  // namespace
#endif  // REX_PLATFORM_MOBILE

int main(int argc, char** argv) {
#if REX_PLATFORM_ANDROID
  // Must run before anything touches guest memory or names a thread: it latches
  // the API level that rex::memory/thread::AndroidInitialize gate their
  // ASharedMemory_create and pthread_getname_np lookups on. SDL's JNI_OnLoad
  // has already run by this point, so the VM and activity are available.
  {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    JavaVM* vm = nullptr;
    if (env) {
      env->GetJavaVM(&vm);
    }
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!rex::InitializeAndroidAppFromMainThread(vm, activity)) {
      std::fprintf(stderr, "Failed to initialize Android app state\n");
      return EXIT_FAILURE;
    }
    if (env && activity) {
      // InitializeAndroidAppFromMainThread took its own global ref.
      env->DeleteLocalRef(activity);
    }
  }
#endif

#if REX_PLATFORM_IOS
  // MoltenVK reads its configuration from the environment when the instance is
  // created, so this has to happen before anything touches Vulkan. Both values
  // are set without overwriting, so exporting either one still wins: bring-up
  // used level 3 with frame tracking, because a blocked nextDrawable is
  // otherwise indistinguishable from a hung renderer.
  //
  // Those defaults are far too expensive to keep once the renderer works.
  // Performance tracking timestamps every command buffer and every encode, and
  // at level 3 MoltenVK narrates each pipeline and dumps a 22-line statistics
  // block every 30 frames - twice a second - through a line-buffered stderr
  // pointed at a file on flash, so each of those lines is its own synchronous
  // write on the render path. Errors still come through at level 1.
  setenv("MVK_CONFIG_PERFORMANCE_TRACKING", "0", 0);

  // Left synchronous deliberately, having measured the alternative.
  //
  // Encoding a VkCommandBuffer into a MTLCommandBuffer costs 13-16 ms a frame
  // here and MoltenVK does it inline in vkQueueSubmit, on the thread that must
  // keep feeding the emulated GPU - so moving it to MoltenVK's own queue looks
  // like the obvious win. It is not: MoltenVK holds a device-level lock across
  // the encode, so creating any resource on the render thread then blocks for
  // as long as the encode takes. On device that turned a descriptor-set
  // allocation, normally microseconds, into a 16 ms stall - one whole encode -
  // landing at random inside frames. Average throughput improved slightly and
  // frame-to-frame consistency, which is what a locked frame rate is made of,
  // got much worse.
  //
  // Encode on the critical path is a known, even cost that fits the budget.
  // Revisit if the resource creation that collides with it moves off the
  // render thread.
  //
  // There is now a reason to revisit it that has nothing to do with throughput.
  // Encoding on the submitting thread means beginning a render pass on the
  // swapchain image calls getCAMetalDrawable() there, and when every drawable
  // is in flight that blocks - about a second, and then fails. Caught in the
  // act by the hang watchdog, main thread parked in __semwait_signal under
  // MVKPresentableSwapchainImage::getCAMetalDrawable() inside MVKQueue::submit,
  // and the run ended "Failed to submit a Vulkan command buffer" followed by
  // the device reported lost. It is not a lost device; it is a wait that
  // expired, on the one thread that must not wait.
  //
  // Both of these are set below rather than here, so that they can be changed
  // from Documents/user/ios_args.txt without a rebuild - the answer to which
  // setting is right is measured on device, and the measurement costs half an
  // hour if it needs a build. MoltenVK reads its configuration when the
  // instance is created, which is long after the arguments are parsed.

  // MoltenVK reports through stderr, which iOS simply discards for a GUI app,
  // so its diagnostics are invisible unless stderr is given somewhere to go.
  // The crash reporter writes here too, which is why this happens before any
  // of it runs.
  {
    const char* home_dir = std::getenv("HOME");
    const std::filesystem::path stderr_path =
        std::filesystem::path(home_dir ? home_dir : ".") / "Documents" / "stderr.log";
    std::freopen(stderr_path.c_str(), "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);
  }

  // Splice the derived arguments in ahead of anything the launcher passed.
  const std::vector<std::string> ios_args = BuildIOSArguments();
  std::vector<char*> ios_argv;
  ios_argv.reserve(size_t(argc) + ios_args.size());
  ios_argv.push_back(argc > 0 ? argv[0] : const_cast<char*>("skate3"));
  for (const std::string& arg : ios_args) {
    ios_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  for (int i = 1; i < argc; ++i) {
    ios_argv.push_back(argv[i]);
  }
  argc = int(ios_argv.size());
  argv = ios_argv.data();
#endif

#if REX_PLATFORM_ANDROID
  const std::filesystem::path android_root = AndroidFilesRoot();
  {
    // user/ is where android_args.txt and the settings live; make sure it
    // exists before anything looks for it.
    std::error_code ec;
    std::filesystem::create_directories(android_root / "user", ec);
    // stderr is discarded in an app process, and the crash reporter and the
    // argument-override diagnostics write there. Give it a file next to the
    // log, line-buffered so a crash loses at most one line.
    const std::filesystem::path stderr_path = android_root / "stderr.log";
    if (std::freopen(stderr_path.c_str(), "w", stderr)) {
      setvbuf(stderr, nullptr, _IOLBF, 0);
    }
  }
  // What the activity passed (SDLActivity.getArguments) is the operator's
  // intent: record it as explicitly set, and let it replace the shipped
  // default for the same key rather than sit beside it - CLI11 rejects a
  // scalar option it sees twice, and that failure discards EVERYTHING.
  std::vector<std::string> activity_keys;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.rfind("--", 0) != 0) {
      continue;
    }
    arg.remove_prefix(2);
    const std::string_view key = arg.substr(0, arg.find('='));
    activity_keys.emplace_back(key);
    rex::cvar::NoteExplicitlySet(key);
  }
  std::vector<std::string> android_args = BuildAndroidArguments();
  std::erase_if(android_args, [&](const std::string& shipped) {
    std::string_view key(shipped);
    key.remove_prefix(2);
    key = key.substr(0, key.find('='));
    return std::find(activity_keys.begin(), activity_keys.end(), key) != activity_keys.end();
  });
  std::vector<char*> android_argv;
  android_argv.reserve(size_t(argc) + android_args.size());
  android_argv.push_back(argc > 0 ? argv[0] : const_cast<char*>("skate3"));
  for (const std::string& arg : android_args) {
    android_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  for (int i = 1; i < argc; ++i) {
    android_argv.push_back(argv[i]);
  }
  argc = int(android_argv.size());
  argv = android_argv.data();
#endif

#if !REX_PLATFORM_MOBILE
  // Everywhere else the command line IS the operator, so record it before it is
  // parsed. Done here rather than inside cvar::Init because on iOS the same
  // argv also carries BuildIOSArguments' defaults, which must not count.
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.rfind("--", 0) != 0) {
      continue;
    }
    arg.remove_prefix(2);
    rex::cvar::NoteExplicitlySet(arg.substr(0, arg.find('=')));
  }
#endif

  auto remaining = rex::cvar::Init(argc, argv);

#if REX_PLATFORM_IOS
  // MoltenVK is configured through the environment and reads it when the
  // instance is created, which has not happened yet. Setting these here rather
  // than before the arguments are parsed is what lets ios_args.txt reach them.
  setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS",
         REXCVAR_GET(vulkan_mvk_synchronous_queue_submits) ? "1" : "0", 1);
  setenv("MVK_CONFIG_LOG_LEVEL", std::to_string(REXCVAR_GET(vulkan_mvk_log_level)).c_str(), 1);
  // The fault that ends sessions is inside MoltenVK's own presentCAMetalDrawable,
  // in the completion block it hands to Metal - it survives our own memory
  // budgets, a deferred swapchain destroy, and answering the system's
  // low-memory warning, and MoltenVK is already at its latest release. The one
  // thing that reliably stops it is synchronous submits, which costs half the
  // frame rate. This is the other route through that code: presenting via a
  // command buffer rather than the drawable, which creates the completion in a
  // different place.
  setenv("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER",
         REXCVAR_GET(vulkan_mvk_present_with_command_buffer) ? "1" : "0", 1);
#endif
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

#if REX_PLATFORM_MAC
  // Use native macOS fullscreen so the menu bar and app switching behave like
  // other Mac apps. MoltenVK presentation is paced separately on macOS.
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY, "1");
#endif

#if REX_PLATFORM_ANDROID
  // Back is the settings-menu key, not "finish the activity".
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
  // The activity keeps input focus while Android recreates the surface, but
  // SDL's keyboard-focus window is null in that interval and, without this,
  // controller button events arriving then are silently discarded.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  // HIDAPI enumerates USB and Bluetooth HID itself; on Android it has been
  // seen to consume a measurable slice of a core while finding nothing the
  // platform's own controller APIs had not already reported.
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
#endif

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "Failed to initialize SDL video: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

#if REX_PLATFORM_MOBILE
  // A controller-driven game sends no touch events, so iOS sees an idle screen
  // and dims, locks, and backgrounds the app out from under a live run. SDL
  // routes this to UIApplication.idleTimerDisabled.
  SDL_DisableScreenSaver();
#endif

  int result = EXIT_FAILURE;
  {
    rex::ui::SDLWindowedAppContext app_context;
    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    bool initialized = app->OnInitialize();
#if REX_PLATFORM_IOS
    // SDL_main runs inside a delayed perform on the main runloop, so the whole
    // game loop executes without ever unwinding back to UIKit. The window and
    // its CAMetalLayer are created during OnInitialize but are not laid out or
    // committed until the runloop turns, and the first frame's nextDrawable
    // blocks against a layer that is not live yet - a deadlock, because the
    // thread that would service the runloop is the one waiting. Turning the
    // runloop here lets UIKit finish committing the scene before any of that.
    // CoreFoundation rather than NSRunLoop: this is a .cpp, and
    // CFRunLoopRunInMode is the same turn of the same runloop.
    if (initialized) {
      for (int i = 0; i < 10; ++i) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
      }
    }
#endif
    result = initialized ? app_context.RunMainLoop() : EXIT_FAILURE;
#if REX_PLATFORM_MAC || REX_PLATFORM_ANDROID
    // Android: the same applies (SDLActivity.onDestroy waits one second for
    // SDL_main to return and the process is torn down regardless).
    //
    // Skip app/runtime teardown entirely: guest threads cannot be reliably
    // stopped on Darwin (pthread_cancel only lands at cancellation points,
    // never in CPU-bound recompiled code), so the destructor chain races
    // still-running guest threads over freed kernel objects and turns a
    // normal quit into the macOS crash-reporter dialog. Flush the logs and
    // leave; the OS reclaims everything else. Flush-only on purpose -
    // ShutdownLogging destroys sinks a straggler thread may still be using.
    rex::FlushLogging();
    std::_Exit(result);
#else
    app->InvokeOnDestroy();
#endif
  }

  rex::ShutdownLogging();
  SDL_Quit();
  return result;
}
