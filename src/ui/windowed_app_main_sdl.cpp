#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include <SDL3/SDL.h>
#if REX_PLATFORM_ANDROID
// Renames main() to SDL_main, which is what SDLActivity's native loader calls.
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <rex/main_android.h>
#endif

#if REX_PLATFORM_IOS
// SDL's iOS entry point also goes through SDL_main.
#include <SDL3/SDL_main.h>
#include <CoreFoundation/CoreFoundation.h>
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
std::vector<std::string> ApplyIOSArgumentOverrides(std::vector<std::string> args,
                                                   const std::filesystem::path& override_file) {
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

  for (const std::string& override_arg : overrides) {
    const std::string key = key_of(override_arg);
    args.erase(std::remove_if(args.begin(), args.end(),
                              [&](const std::string& arg) { return key_of(arg) == key; }),
               args.end());
  }
  args.insert(args.end(), overrides.begin(), overrides.end());

  // Say so. An override that silently fails to apply is indistinguishable from
  // one that applied and did nothing, and telling those apart by watching frame
  // times is exactly the guessing this file exists to avoid. Logging is not up
  // yet at this point - these arguments are what configures it - so this goes
  // to stderr, which iOS has already been pointed at Documents/stderr.log.
  std::fprintf(stderr, "ios_args: applied %zu override(s) from %s\n", overrides.size(),
               override_file.c_str());
  for (const std::string& override_arg : overrides) {
    std::fprintf(stderr, "ios_args:   %s\n", override_arg.c_str());
  }
  std::fflush(stderr);

  return args;
}

std::vector<std::string> BuildIOSArguments() {
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
      // The disc is staged into Documents, so never run the install wizard.
      "--skate3_auto_install_dlc=false",
      // Per-window frame breakdown. One formatted line every 600 guest frames
      // is far too little traffic to distort what it measures, and without it
      // there is no way to tell a build that helped from one that did not.
      "--skate3_native_render_scene_perf_log=true",
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
      // Twice the frame rate is twice the decode, upload and in-flight frame
      // residency, on a 4 GB phone that had no headroom at 30. A locked 30 with
      // half the budget spare is worth more than a 60 that lasts four minutes.
      //
      // Raise it from Documents/user/ios_args.txt (skate3_guest_fps_cap=60)
      // when the residency work lands; no rebuild needed.
      "--skate3_guest_fps_cap=30",
      "--skate3_guest_fps_cap_auto=false",

      // ---- Cache budgets --------------------------------------------------
      // These default to 1280 and 1024 MB, which is 2.3 GB of caches before
      // either LRU evicts anything. That is a desktop budget: on device the
      // measured growth was ~18 MB a second with neither LRU ever starting,
      // reaching 1382 MB of the 2730 MB heap in about half a minute and dying
      // shortly after - and iOS kills the app well before the heap budget is
      // the binding constraint. Textures cost more here than the numbers
      // suggest, too, since Metal exposes no BC formats and every DXT surface
      // is expanded to RGBA8 on upload (8x for DXT1).
      "--skate3_native_render_scene_tex_store_mb=512",
      "--skate3_native_render_scene_mesh_store_mb=384",

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
      // once, which is guest-heap pressure rather than GPU memory. Deliberately
      // NOT set here any more: these were pinned to 1.0, which beats
      // settings.toml, so a player who had asked for 0.75 was silently given
      // the full distance - and the extra residency shows up as texture store
      // above its budget, continuous eviction, and multi-millisecond frames
      // spent destroying what was evicted. Let the file decide.

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
  };

  return ApplyIOSArgumentOverrides(std::move(args), documents / "user" / "ios_args.txt");
}

}  // namespace
#endif

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
  setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
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
  setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", 0);

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

  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

#if REX_PLATFORM_MAC
  // Use native macOS fullscreen so the menu bar and app switching behave like
  // other Mac apps. MoltenVK presentation is paced separately on macOS.
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
  SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY, "1");
#endif

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "Failed to initialize SDL video: %s\n", SDL_GetError());
    return EXIT_FAILURE;
  }

#if REX_PLATFORM_IOS
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
#if REX_PLATFORM_MAC
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
