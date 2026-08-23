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
#include <cstdlib>

namespace {

// iOS launches an app with no arguments, and there is no Java shell to build
// them the way Skate3Activity does on Android, so the paths are derived here.
// getenv("HOME") is the app sandbox root under iOS, making $HOME/Documents the
// user-visible directory that files copied in via Finder or iTunes land in.
std::vector<std::string> BuildIOSArguments() {
  const char* home = std::getenv("HOME");
  const std::filesystem::path documents =
      std::filesystem::path(home ? home : ".") / "Documents";
  return {
      "--game_data_root=" + (documents / "game").string(),
      "--user_data_root=" + (documents / "user").string(),
      "--log_file=" + (documents / "skate3.log").string(),
      "--log_flush_interval=1",
      // The emulated Xenos backend needs geometry shaders, which Metal (and so
      // MoltenVK) does not expose; the Skate-3 native renderer replaces it.
      "--skate3_native_render_scene=true",
      "--vulkan_require_geometry_shader=false",
      "--vulkan_require_fill_mode_non_solid=false",
      // Routes MoltenVK's own reports through the debug messenger into the
      // normal log, where they sit next to the frame they belong to.
      "--vulkan_log_debug_messages=true",
      // The disc is staged into Documents, so never run the install wizard.
      "--skate3_auto_install_dlc=false",
      // Mitigation for the WorldPresentation cross-thread use-after-free.
      "--skate3_instance_free_defer_ms=250",

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
  };
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
