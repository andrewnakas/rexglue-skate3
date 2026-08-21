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
      // The disc is staged into Documents, so never run the install wizard.
      "--skate3_auto_install_dlc=false",
      // Mitigation for the WorldPresentation cross-thread use-after-free.
      "--skate3_instance_free_defer_ms=250",
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
